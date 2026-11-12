#include <dsfilemanager/filemanager.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCoroTask>
#include <iostream>
#include <sstream>
#include <logfault/logfault.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace dsfm;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #__VA_ARGS__); } while (false)
template<typename T> T get(QCoro::Task<Result<T>> task) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result) throw std::runtime_error("error=" + std::to_string(int(result.error().code)) + " native=" + std::to_string(result.error().native_code));
    return std::move(result).value();
}
void get(QCoro::Task<Result<void>> task) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result) throw std::runtime_error("error=" + std::to_string(int(result.error().code)) + " native=" + std::to_string(result.error().native_code));
}
template<typename T> void error(QCoro::Task<Result<T>> task, ErrorCode expected) {
    const auto result = QCoro::waitFor(std::move(task));
    CHECK(!result);
    if (result.error().code != expected) throw std::runtime_error("expected=" + std::to_string(int(expected)) + " actual=" + std::to_string(int(result.error().code)));
}
QByteArray digest(QByteArray data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
struct Fixture {
    QTemporaryDir directory;
    NamespaceConfig config;
    NativeFileManager manager;
    NamespaceInfo ns;
    explicit Fixture(PhysicalNaming naming = PhysicalNaming::OriginalName, NamespaceLimits limits = {})
        : config{QUuid::createUuid(), directory.path() + "/store", naming, limits}, ns(get(manager.createNamespace(config))) {}
    ~Fixture() { QCoro::waitFor(manager.shutdown()); }
    PendingFile pending(QString name, QByteArray bytes, DirectoryId parent = {}) {
        return get(manager.createFile(config.id, {parent.isNull() ? ns.root : parent, name, ConflictPolicy::UniqueName, {quint64(bytes.size()), digest(bytes)}}));
    }
    FileId publish(QString name, QByteArray bytes, DirectoryId parent = {}) {
        auto file = pending(name, bytes, parent); get(file.writer->writeAt(0, bytes)); get(file.writer->commit()); return file.info.file_id;
    }
    QByteArray read(FileId id) {
        auto reader = get(manager.openRead(config.id, id)); auto result = get(reader->readAt(0, config.limits.max_io_bytes)); get(reader->close()); return result;
    }
    QString path(FileId id) {
        if (config.naming == PhysicalNaming::FileId) { const auto name = id.toString(QUuid::WithoutBraces); return config.storage_root + "/objects/" + name.left(2) + '/' + name; }
        return config.storage_root + '/' + get(manager.stat(config.id, id)).name;
    }
};
int child(const QStringList& args) {
    const auto mode = args[2];
    NamespaceConfig config{QUuid(args[4]), args[3], PhysicalNaming(args[5].toInt())};
    NativeFileManager manager;
    auto opened = QCoro::waitFor(manager.openNamespace(config));
    if (mode == "busy") return !opened && opened.error().code == ErrorCode::NamespaceBusy ? 0 : 40;
    if (!opened) return 41;
    const auto ns = opened.value();
    const auto point = args.value(6).toLatin1();
    if (mode == "create") {
        auto file = get(manager.createFile(config.id, {ns.root, "new.txt", ConflictPolicy::Fail, {3, digest("new")}}));
        get(file.writer->writeAt(0, "new")); get(file.writer->checkpoint());
        qputenv("DSFM_CRASH_AT", point); get(file.writer->commit());
    } else if (mode == "abort") {
        auto file = get(manager.createFile(config.id, {ns.root, "abort.bin", ConflictPolicy::Fail, {3, digest("new")}}));
        get(file.writer->writeAt(0, "new")); get(file.writer->checkpoint());
        qputenv("DSFM_CRASH_AT", point); get(file.writer->abort());
    } else if (mode == "checkpoint") {
        auto file = get(manager.createFile(config.id, {ns.root, "partial.txt", ConflictPolicy::Fail, {6, digest("abcdef")}}));
        get(file.writer->writeAt(0, "abc")); get(file.writer->markVerified({0,3}));
        qputenv("DSFM_CRASH_AT", point); get(file.writer->writeAt(0, "xyz")); get(file.writer->checkpoint());
    } else {
        const auto entry = get(manager.lookup(config.id, "base.txt"));
        if (mode == "replace") {
            auto file = get(manager.replaceFile(config.id, entry.id, {3, digest("new")})); get(file.writer->writeAt(0, "new"));
            qputenv("DSFM_CRASH_AT", point); get(file.writer->commit());
        } else if (mode == "rename") { qputenv("DSFM_CRASH_AT", point); get(manager.rename(config.id, entry.id, "renamed.txt")); }
        else if (mode == "move") {
            const auto dir = get(manager.lookup(config.id, "target"));
            qputenv("DSFM_CRASH_AT", point); get(manager.move(config.id, entry.id, dir.id));
        } else if (mode == "delete") { qputenv("DSFM_CRASH_AT", point); get(manager.remove(config.id, entry.id)); }
    }
    get(manager.shutdown()); return 42; // A crash-mode child must terminate at its selected boundary.
}
void runChild(QString mode, const NamespaceConfig& config, QByteArray point = {}, int expected = 86) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QCoreApplication::applicationFilePath(), {"--child", mode, config.storage_root, config.id.toString(), QString::number(int(config.naming)), QString::fromLatin1(point)});
    CHECK(process.waitForStarted(5000)); CHECK(process.waitForFinished(20000));
    if (process.exitCode() != expected) throw std::runtime_error("child " + mode.toStdString() + " " + point.toStdString() + " exit=" + std::to_string(process.exitCode()) + " " + process.readAll().toStdString());
}
void names() {
    const QStringList bad{"", ".", "..", "../x", "a/b", "a\\b", "CON", "con.txt", "LPT9.x", "COM¹.txt", "aux", "a:", "a.", "a ", ".filemanager", "OBJECTS", QString(256, 'x'), QString(128, QChar(0x00e9)), QString("a\n"), QString(QChar(0xd800)), QString("a") + QChar(0x202e)};
    for (auto name : bad) CHECK(!normalizeName(name));
    CHECK(normalizeName("report.pdf").value() == "report.pdf");
    CHECK(normalizeName(QString::fromUtf8("e\xcc\x81.txt")).value() == QString::fromUtf8("é.txt"));
    CHECK(nameKey("Straße.txt").value() == nameKey("STRASSE.TXT").value());
    CHECK(normalizeName(QString(255, 'x')));
}
void lifecycle(PhysicalNaming naming) {
    Fixture f(naming);
    auto dir = get(f.manager.mkdir(f.config.id, f.ns.root, "Received"));
    auto file = f.pending("report.pdf", "hello");
    error(f.manager.openRead(f.config.id, file.info.file_id), ErrorCode::MissingEntry);
    get(file.writer->writeAt(0, "hello")); get(file.writer->markVerified({0,5}));
    auto ranges = get(f.manager.listRanges(f.config.id, file.info.id, RangeKind::ApplicationVerified)); CHECK(ranges.entries == QVector<ByteRange>{{0,5}});
    get(file.writer->commit()); CHECK(f.read(file.info.file_id) == "hello");
    auto second = f.pending("REPORT.PDF", "two"); CHECK(second.info.name == "REPORT (2).PDF"); get(second.writer->abort());
    get(f.manager.rename(f.config.id, file.info.file_id, "renamed.pdf"));
    get(f.manager.move(f.config.id, file.info.file_id, dir));
    CHECK(get(f.manager.lookup(f.config.id, "received/renamed.pdf")).id == file.info.file_id);
    CHECK(f.read(file.info.file_id) == "hello");
    error(f.manager.remove(f.config.id, dir), ErrorCode::DirectoryNotEmpty);
    get(f.manager.remove(f.config.id, file.info.file_id)); get(f.manager.remove(f.config.id, dir));
    CHECK(get(f.manager.list(f.config.id, f.ns.root)).entries.isEmpty());
}
void resume(PhysicalNaming naming) {
    Fixture f(naming);
    auto file = f.pending("resume.bin", "abcdef");
    get(file.writer->writeAt(3, "def")); get(file.writer->markVerified({3,3}));
    error(file.writer->commit(), ErrorCode::IncompleteContent);
    get(file.writer->suspend());
    get(f.manager.shutdown());
    NativeFileManager reopened; auto info = get(reopened.openNamespace(f.config)); CHECK(info.root == f.ns.root);
    auto pending = get(reopened.listPending(f.config.id)); CHECK(pending.entries.size() == 1);
    auto resumed = get(reopened.openPending(f.config.id, file.info.id));
    CHECK(get(reopened.listRanges(f.config.id, file.info.id, RangeKind::StorageValid)).entries == QVector<ByteRange>{{3,3}});
    get(resumed.writer->writeAt(0, "abc")); get(resumed.writer->commit());
    auto reader = get(reopened.openRead(f.config.id, file.info.file_id)); CHECK(get(reader->readAt(0, 20)) == "abcdef"); get(reader->close());
    get(reopened.shutdown());
}
void integrity() {
    Fixture f;
    auto file = f.pending("integrity", "abcdef");
    get(file.writer->writeAt(0, "abcxxx")); error(file.writer->commit(), ErrorCode::IntegrityError);
    error(f.manager.openRead(f.config.id, file.info.file_id), ErrorCode::MissingEntry);
    CHECK(get(f.manager.listPending(f.config.id)).entries[0].state == PendingState::IntegrityFailed);
    get(file.writer->writeAt(3, "def")); get(file.writer->commit()); CHECK(f.read(file.info.file_id) == "abcdef");
    auto unknown = get(f.manager.createFile(f.config.id, {f.ns.root, "unknown"}));
    get(unknown.writer->writeAt(4, "x")); get(unknown.writer->setFinalSize(5));
    error(unknown.writer->commit(), ErrorCode::IncompleteContent);
    get(unknown.writer->writeAt(0, "abcd")); get(unknown.writer->commit()); CHECK(f.read(unknown.info.file_id) == "abcdx");
    auto empty = f.pending("empty", ""); get(empty.writer->commit()); CHECK(f.read(empty.info.file_id).isEmpty());
}
void replacement(PhysicalNaming naming) {
    Fixture f(naming); auto id = f.publish("old", "old");
    auto reader = get(f.manager.openRead(f.config.id, id));
    auto pending = get(f.manager.replaceFile(f.config.id, id, {3, digest("new")})); CHECK(pending.info.file_id == id);
    get(pending.writer->writeAt(0, "new")); error(pending.writer->commit(), ErrorCode::Busy);
    CHECK(get(reader->readAt(0, 10)) == "old"); get(reader->close()); get(pending.writer->commit()); CHECK(f.read(id) == "new");
    CHECK(get(f.manager.list(f.config.id, f.ns.root)).entries.size() == 1);
    auto aborted = get(f.manager.replaceFile(f.config.id, id, {3, digest("bad")})); get(aborted.writer->abort()); CHECK(f.read(id) == "new");
}
void reservationAndDirectories() {
    Fixture f; auto dir = get(f.manager.mkdir(f.config.id, f.ns.root, "dir"));
    auto child = get(f.manager.mkdir(f.config.id, dir, "child"));
    error(f.manager.move(f.config.id, dir, child), ErrorCode::InvalidArgument);
    error(f.manager.move(f.config.id, dir, QUuid::createUuid()), ErrorCode::CrossNamespaceMove);
    auto file = f.pending("pending", "x", child);
    error(f.manager.rename(f.config.id, dir, "new"), ErrorCode::Busy);
    error(f.manager.openPending(f.config.id, file.info.id), ErrorCode::Busy);
    get(file.writer->suspend()); error(f.manager.rename(f.config.id, dir, "new"), ErrorCode::Busy);
    auto resumed = get(f.manager.openPending(f.config.id, file.info.id)); get(resumed.writer->abort());
    get(f.manager.rename(f.config.id, dir, "new"));
    error(f.manager.rename(f.config.id, f.ns.root, "root"), ErrorCode::InvalidArgument);
}
void quotas() {
    NamespaceLimits limits; limits.max_file_size = 8; limits.max_total_bytes = 12; limits.max_staging_bytes = 8; limits.max_pending = 1; limits.max_io_bytes = 4;
    Fixture f(PhysicalNaming::FileId, limits);
    auto file = f.pending("bounded", "12345678");
    error(f.manager.createFile(f.config.id, {f.ns.root, "overflow"}), ErrorCode::QuotaExceeded);
    error(file.writer->writeAt(0, "12345"), ErrorCode::QuotaExceeded);
    error(file.writer->writeAt(std::numeric_limits<quint64>::max(), "x"), ErrorCode::QuotaExceeded);
    get(file.writer->writeAt(0, "1234")); get(file.writer->writeAt(4, "5678")); get(file.writer->commit());
    error(f.manager.replaceFile(f.config.id, file.info.file_id, {8,digest("abcdefgh")}), ErrorCode::QuotaExceeded);
    auto unknown = get(f.manager.createFile(f.config.id, {f.ns.root, "unknown"}));
    error(unknown.writer->writeAt(4, "xxxx"), ErrorCode::QuotaExceeded); get(unknown.writer->abort());
}
void listings() {
    Fixture f;
    for (auto name : {"c", "a", "b"}) f.publish(name, name);
    auto page = get(f.manager.list(f.config.id, f.ns.root, {1,{}})); CHECK(!page.entries.isEmpty()); CHECK(page.entries[0].name == "a"); CHECK(!page.next_cursor.isEmpty());
    auto next = get(f.manager.list(f.config.id, f.ns.root, {1,page.next_cursor})); CHECK(next.entries[0].name == "b");
    f.publish("d", "d"); error(f.manager.list(f.config.id, f.ns.root, {1,page.next_cursor}), ErrorCode::StaleCursor);
    error(f.manager.list(f.config.id, f.ns.root, {0,{}}), ErrorCode::InvalidArgument);
    error(f.manager.list(f.config.id, f.ns.root, {1000,{}}), ErrorCode::InvalidArgument);
    error(f.manager.list(f.config.id, f.ns.root, {1,QByteArray(5000,'x')}), ErrorCode::InvalidArgument);
}
void confinement() {
    Fixture f;
    auto dir = get(f.manager.mkdir(f.config.id, f.ns.root, "dir"));
    CHECK(QDir().rmdir(f.config.storage_root + "/dir")); CHECK(::symlink(f.directory.path().toUtf8(), (f.config.storage_root + "/dir").toUtf8()) == 0);
    auto created = QCoro::waitFor(f.manager.createFile(f.config.id, {dir, "escape"})); CHECK(!created); CHECK(!QFile::exists(f.directory.path() + "/escape"));
    auto id = f.publish("hardlink", "data");
    CHECK(::link(f.path(id).toUtf8(), (f.directory.path()+"/alias").toUtf8()) == 0);
    error(f.manager.openRead(f.config.id, id), ErrorCode::ExternalModification);
    auto second = NativeFileManager{};
    error(second.openNamespace(f.config), ErrorCode::NamespaceBusy);
    NamespaceConfig nested{QUuid::createUuid(), f.config.storage_root + "/nested"};
    error(second.createNamespace(nested), ErrorCode::Conflict);
    CHECK(!QFile::exists(nested.storage_root + "/.filemanager"));
    runChild("busy", f.config, {}, 0);
    get(second.shutdown());
}
void externalChanges() {
    Fixture f;
    auto id = f.publish("modified", "secret"); auto reader = get(f.manager.openRead(f.config.id, id));
    QFile file(f.path(id)); CHECK(file.open(QIODevice::WriteOnly)); CHECK(file.write("tampered") == 8); file.close();
    error(reader->readAt(0, 100), ErrorCode::ExternalModification); get(reader->close());
    CHECK(get(f.manager.stat(f.config.id, id)).status == EntryStatus::Inconsistent);
    error(f.manager.openRead(f.config.id, id), ErrorCode::ExternalModification);
    auto missing = f.publish("missing", "x"); CHECK(QFile::remove(f.path(missing)));
    CHECK(get(f.manager.stat(f.config.id, missing)).status == EntryStatus::Missing);
    get(f.manager.reconcile(f.config.id, {ReconciliationAction::DiscardMissing, missing, {}}));
    error(f.manager.stat(f.config.id, missing), ErrorCode::MissingEntry);
}
void cancellationAndNotifications() {
    Fixture f; quint32 notifications = 0; QObject context;
    auto subscription = f.manager.subscribe(f.config.id, &context, [&](Change change) { CHECK(change.rescan_required); ++notifications; }); CHECK(subscription);
    auto file = f.pending("cancel", "bytes");
    CancellationToken token; token.cancel(); error(file.writer->writeAt(0, "bytes", token), ErrorCode::Cancelled);
    get(file.writer->writeAt(0, "bytes")); error(file.writer->commit(token), ErrorCode::Cancelled);
    CHECK(get(f.manager.listPending(f.config.id)).entries.size() == 1); get(file.writer->commit());
    QCoreApplication::processEvents(); CHECK(notifications > 0); f.manager.unsubscribe(subscription.value());
    auto stopped = f.manager.shutdown(); get(std::move(stopped)); error(f.manager.stat(f.config.id, file.info.file_id), ErrorCode::ShuttingDown);
}
void crashRecovery() {
    for (auto naming : {PhysicalNaming::OriginalName, PhysicalNaming::FileId}) {
        for (auto mode : {"create", "replace", "rename", "move", "delete", "abort"}) {
            if (naming == PhysicalNaming::FileId && (QByteArray(mode) == "rename" || QByteArray(mode) == "move")) continue;
            for (auto point : {"after_prepare", "after_filesystem", "after_filesystem_record", "after_catalog", "after_cleanup"}) {
                Fixture f(naming); auto id = f.publish("base.txt", "old"); get(f.manager.mkdir(f.config.id, f.ns.root, "target")); get(f.manager.shutdown());
                runChild(mode, f.config, point);
                NativeFileManager recovered; get(recovered.openNamespace(f.config));
                const auto name = QByteArray(mode) == "create" ? "new.txt" : QByteArray(mode) == "rename" ? "renamed.txt" : QByteArray(mode) == "move" ? "target/base.txt" : "base.txt";
                if (QByteArray(mode) == "delete") error(recovered.lookup(f.config.id, name), ErrorCode::MissingEntry);
                else {
                    auto entry = get(recovered.lookup(f.config.id, name));
                    if (QByteArray(mode) != "create") CHECK(entry.id == id);
                    auto reader = get(recovered.openRead(f.config.id, entry.id)); auto bytes = get(reader->readAt(0, 10)); get(reader->close());
                    CHECK(bytes == ((QByteArray(mode) == "create" || QByteArray(mode) == "replace") ? "new" : "old"));
                }
                get(recovered.shutdown());
                NativeFileManager again; get(again.openNamespace(f.config)); get(again.shutdown());
            }
        }
    }
}
void overwriteCrash() {
    for (auto point : {"after_range_invalidation", "before_checkpoint_sync", "after_checkpoint_sync", "after_checkpoint_record"}) {
        Fixture f; get(f.manager.shutdown()); runChild("checkpoint", f.config, point);
        NativeFileManager recovered; get(recovered.openNamespace(f.config));
        auto pending = get(recovered.listPending(f.config.id)); CHECK(pending.entries.size() == 1);
        const auto valid = get(recovered.listRanges(f.config.id, pending.entries[0].id, RangeKind::StorageValid)).entries;
        if (QByteArray(point) == "after_checkpoint_record") CHECK(valid == QVector<ByteRange>{{0,3}});
        else CHECK(valid.isEmpty());
        CHECK(get(recovered.listRanges(f.config.id, pending.entries[0].id, RangeKind::ApplicationVerified)).entries.isEmpty());
        auto file = get(recovered.openPending(f.config.id, pending.entries[0].id)); error(file.writer->commit(), ErrorCode::IncompleteContent); get(file.writer->abort()); get(recovered.shutdown());
    }
}
void resumedTampering() {
    Fixture f;
    auto pending = f.pending("tampered", "abcdef");
    get(pending.writer->writeAt(0, "abcdef")); get(pending.writer->markVerified({0,6})); get(pending.writer->suspend()); get(f.manager.shutdown());
    QFile staging(f.config.storage_root + "/.filemanager/staging/" + pending.info.id.toString(QUuid::WithoutBraces));
    CHECK(staging.open(QIODevice::WriteOnly)); CHECK(staging.write("ABCDEF") == 6); staging.close();
    NativeFileManager reopened; get(reopened.openNamespace(f.config)); auto file = get(reopened.openPending(f.config.id, pending.info.id));
    error(file.writer->commit(), ErrorCode::IntegrityError);
    error(reopened.openRead(f.config.id, pending.info.file_id), ErrorCode::MissingEntry);
    get(file.writer->writeAt(0, "abcdef")); get(file.writer->commit()); get(reopened.shutdown());
}
void fragmentation() {
    NamespaceLimits limits; limits.max_ranges = 2;
    Fixture f(PhysicalNaming::FileId, limits);
    auto file = f.pending("fragmented", "abcde");
    get(file.writer->writeAt(0, "a")); get(file.writer->writeAt(2, "c"));
    error(file.writer->writeAt(4, "e"), ErrorCode::QuotaExceeded);
    get(file.writer->writeAt(1, "b")); get(file.writer->writeAt(3, "de")); get(file.writer->commit()); CHECK(f.read(file.info.file_id) == "abcde");
}
void concurrencyAndShutdown() {
    Fixture f;
    auto first = f.pending("first", "abc"); auto second = f.pending("second", "xyz");
    auto write_a = first.writer->writeAt(0, "a"); auto write_b = first.writer->writeAt(1, "bc"); auto write_c = second.writer->writeAt(0, "xyz");
    get(std::move(write_a)); get(std::move(write_b)); get(std::move(write_c));
    get(first.writer->commit()); get(second.writer->commit());
    auto reader = get(f.manager.openRead(f.config.id, first.info.file_id));
    quint32 heartbeats = 0; QTimer timer; QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeats; }); timer.start(0);
    for (quint32 i = 0; i < 20; ++i) CHECK(get(reader->readAt(0, 10)) == "abc");
    CHECK(heartbeats > 0); timer.stop();
    get(f.manager.shutdown());
    NativeFileManager reopened; get(reopened.openNamespace(f.config)); get(reopened.shutdown());
}
void schemaAndPermissions() {
    Fixture f; f.publish("fixture", "v1"); auto file = f.pending("private", "x"); get(file.writer->suspend()); get(f.manager.shutdown());
    for (auto relative : {"", "/.filemanager", "/.filemanager/staging", "/.filemanager/recovery", "/.filemanager/catalog.db"}) {
        struct stat st{}; CHECK(::lstat((f.config.storage_root + relative).toUtf8(), &st) == 0); CHECK((st.st_mode & 0077) == 0);
    }
    QFile fixture(QStringLiteral(DSFM_TEST_SOURCE_DIR) + "/fixtures/schema-v1.json"); CHECK(fixture.open(QIODevice::ReadOnly));
    const auto expected = QJsonDocument::fromJson(fixture.readAll()).object();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "fixture"); db.setDatabaseName(f.config.storage_root + "/.filemanager/catalog.db"); CHECK(db.open());
        QSqlQuery query(db); CHECK(query.exec("SELECT value FROM meta WHERE key='schema'")); CHECK(query.next()); CHECK(query.value(0).toString() == expected["version"].toString());
        const auto tables = expected["tables"].toObject();
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            CHECK(query.exec("PRAGMA table_info(" + it.key() + ')')); QJsonArray actual; while (query.next()) actual.append(query.value(1).toString()); CHECK(actual == it.value().toArray());
        }
        CHECK(query.exec("SELECT phase,payload FROM operations"));
        quint32 operation_count = 0;
        while (query.next()) {
            ++operation_count;
            CHECK(expected["phases"].toArray().contains(query.value(0).toString()));
            const auto op = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
            CHECK(op["version"].toInt() == expected["operation_version"].toInt());
            for (auto field : expected["operation_required_fields"].toArray()) CHECK(op.contains(field.toString()));
        }
        CHECK(operation_count > 0);
        CHECK(query.exec("UPDATE meta SET value='99' WHERE key='schema'")); query.finish(); db.close();
    }
    QSqlDatabase::removeDatabase("fixture");
    NativeFileManager unsupported; error(unsupported.openNamespace(f.config), ErrorCode::Corruption); get(unsupported.shutdown());
}
void largeSparseAndBounds() {
    Fixture f(PhysicalNaming::FileId);
    constexpr quint64 size = (4ULL << 30) + 7;
    auto file = get(f.manager.createFile(f.config.id, {f.ns.root, "large"}));
    get(file.writer->writeAt(size-1, "x")); get(file.writer->setFinalSize(size)); get(file.writer->checkpoint());
    auto ranges = get(f.manager.listRanges(f.config.id, file.info.id, RangeKind::StorageValid)); CHECK(ranges.entries == QVector<ByteRange>{{size-1,1}});
    error(file.writer->commit(), ErrorCode::IncompleteContent); get(file.writer->abort());
    auto id = f.publish("small", "x"); auto reader = get(f.manager.openRead(f.config.id, id));
    error(reader->readAt(0, -1), ErrorCode::InvalidArgument); CHECK(get(reader->readAt(100, 1)).isEmpty()); get(reader->close());
}
void injectedFailures() {
    Fixture f;
    auto file = f.pending("short", "abcde");
    qputenv("DSFM_SHORT_WRITE", "2"); CHECK(get(file.writer->writeAt(0, "abcde")) == 2); qunsetenv("DSFM_SHORT_WRITE");
    error(file.writer->commit(), ErrorCode::IncompleteContent);
    qputenv("DSFM_FAIL_AT", "before_write"); error(file.writer->writeAt(2, "cde"), ErrorCode::DiskFull); qunsetenv("DSFM_FAIL_AT");
    get(file.writer->writeAt(2, "cde"));
    qputenv("DSFM_FAIL_AT", "before_checkpoint_sync"); error(file.writer->checkpoint(), ErrorCode::DiskFull); qunsetenv("DSFM_FAIL_AT");
    qputenv("DSFM_FAIL_AT", "publication_sync"); auto committed = get(file.writer->commit()); qunsetenv("DSFM_FAIL_AT");
    CHECK(committed.durability == Durability::SyncFailed);
    const auto status = get(f.manager.operationStatus(f.config.id, committed.operation_id)); CHECK(status.phase == "DurabilityPending"); CHECK(status.result.has_value()); CHECK(status.result->durability == Durability::SyncFailed);
    CHECK(f.read(file.info.file_id) == "abcde");
    CHECK(get(f.manager.operationStatus(f.config.id, committed.operation_id)).phase == "Complete");
    auto conflict = f.pending("conflict", "x"); get(conflict.writer->writeAt(0,"x"));
    QFile external(f.config.storage_root + "/conflict"); CHECK(external.open(QIODevice::WriteOnly)); external.write("foreign"); external.close();
    error(conflict.writer->commit(), ErrorCode::Conflict); CHECK(get(f.manager.listPending(f.config.id)).entries.size() == 1);
    CHECK(QFile::remove(external.fileName())); get(conflict.writer->commit());
}
void reducedLimits() {
    Fixture f; auto id = f.publish("retained", "12345678"); get(f.manager.shutdown());
    auto limits = f.config.limits; limits.max_total_bytes = 8; limits.max_staging_bytes = 8; limits.max_file_size = 8;
    auto config = f.config; config.limits_override = limits;
    NativeFileManager reopened; auto ns = get(reopened.openNamespace(config)); CHECK(ns.charged_bytes == 8);
    error(reopened.createFile(config.id, {ns.root,"new",ConflictPolicy::Fail,{1,digest("x")}}), ErrorCode::QuotaExceeded);
    auto reader = get(reopened.openRead(config.id, id)); CHECK(get(reader->readAt(0,20)) == "12345678"); get(reader->close()); get(reopened.remove(config.id,id)); get(reopened.shutdown());
}
void missingPendingAndOrphans() {
    Fixture f;
    auto file = f.pending("missing", "abc"); get(file.writer->suspend()); get(f.manager.shutdown());
    CHECK(QFile::remove(f.config.storage_root + "/.filemanager/staging/" + file.info.id.toString(QUuid::WithoutBraces)));
    QFile foreign(f.config.storage_root + "/.filemanager/staging/unowned"); CHECK(foreign.open(QIODevice::WriteOnly)); foreign.write("untouched"); foreign.close();
    NativeFileManager reopened; auto info = get(reopened.openNamespace(f.config)); CHECK(info.untracked_staging == 1);
    CHECK(get(reopened.listPending(f.config.id)).entries[0].state == PendingState::Inconsistent);
    error(reopened.openPending(f.config.id,file.info.id), ErrorCode::ExternalModification);
    get(reopened.abortPending(f.config.id,file.info.id)); CHECK(get(reopened.listPending(f.config.id)).entries.isEmpty()); CHECK(QFile::exists(foreign.fileName())); get(reopened.shutdown());
}
void abandonedHandlesAndCancellation() {
    Fixture f;
    auto pending = f.pending("abandoned", "abc");
    auto write = pending.writer->writeAt(0, "abc"); pending.writer.reset(); CHECK(get(std::move(write)) == 3);
    get(f.manager.shutdown());
    NativeFileManager reopened; get(reopened.openNamespace(f.config)); auto recovered = get(reopened.openPending(f.config.id, pending.info.id));
    get(recovered.writer->abort()); get(reopened.shutdown());
    NativeFileManager hashing; auto ns = get(hashing.openNamespace(f.config));
    QByteArray bytes(512*1024,'x'); auto file = get(hashing.createFile(f.config.id, {ns.root,"cancel-hash",ConflictPolicy::Fail,{quint64(bytes.size()),digest(bytes)}}));
    get(file.writer->writeAt(0,bytes)); CancellationToken token;
    QTimer::singleShot(0, [token] { token.cancel(); }); error(file.writer->commit(token), ErrorCode::Cancelled);
    error(hashing.openRead(f.config.id,file.info.file_id), ErrorCode::MissingEntry); get(file.writer->abort()); get(hashing.shutdown());
}
void raceAndPermissions() {
    Fixture f;
    auto file = f.pending("raced", "x"); get(file.writer->writeAt(0,"x"));
    const auto path = f.config.storage_root + "/raced";
    qputenv("DSFM_COLLISION_PATH",path.toUtf8()); error(file.writer->commit(),ErrorCode::Conflict); qunsetenv("DSFM_COLLISION_PATH");
    QFile foreign(path); CHECK(foreign.open(QIODevice::ReadOnly)); CHECK(foreign.readAll() == "foreign"); foreign.close();
    CHECK(QFile::remove(path)); get(file.writer->commit());
    CHECK(::chmod(f.config.storage_root.toUtf8(),0755) == 0);
    error(f.manager.namespaceInfo(f.config.id), ErrorCode::PermissionDenied);
    CHECK(::chmod(f.config.storage_root.toUtf8(),0700) == 0);
}
void backpressure() {
    Fixture f(PhysicalNaming::FileId); QByteArray bytes(1<<20,'x'); auto file = f.pending("pressure",bytes);
    std::vector<QCoro::Task<Result<qsizetype>>> tasks;
    for (quint32 i = 0; i < 64; ++i) tasks.push_back(file.writer->writeAt(0,bytes));
    quint32 rejected = 0, accepted = 0;
    for (auto& task : tasks) {
        auto result = QCoro::waitFor(std::move(task));
        if (result) { ++accepted; CHECK(result.value() == bytes.size()); }
        else { CHECK(result.error().code == ErrorCode::QueueFull); ++rejected; }
    }
    CHECK(accepted > 0 && rejected > 0); get(file.writer->commit());
}
void diagnostics() {
    std::ostringstream output;
    struct Clear { ~Clear() { logfault::LogManager::Instance().ClearHandlers(); } } clear;
    logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(output,logfault::LogLevel::TRACE));
    Fixture f; auto id = f.publish("PRIVATE-FILENAME", "PRIVATE-CONTENT");
    CancellationToken token; token.cancel(); auto pending = f.pending("PRIVATE-OTHER", "x"); error(pending.writer->commit(token), ErrorCode::Cancelled); get(pending.writer->abort());
    get(f.manager.shutdown());
    const auto logs = output.str(); CHECK(logs.find("FileManager") != std::string::npos);
    for (auto private_value : {std::string("PRIVATE-"), f.config.storage_root.toStdString(), id.toString().toStdString(), f.config.id.toString().toStdString()}) CHECK(logs.find(private_value) == std::string::npos);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--child") { try { return child(app.arguments()); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 99; } }
    int failures = 0;
    auto test = [&](const char* name, auto function) { try { function(); std::cout << "PASS " << name << std::endl; } catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << std::endl; } };
    test("names", names);
    for (auto naming : {PhysicalNaming::OriginalName, PhysicalNaming::FileId}) {
        test("lifecycle", [&] { lifecycle(naming); }); test("resume", [&] { resume(naming); }); test("replacement", [&] { replacement(naming); });
    }
    test("integrity", integrity); test("reservations/directories", reservationAndDirectories); test("quotas", quotas); test("listings", listings);
    test("confinement/ownership", confinement); test("external changes", externalChanges); test("cancellation/notifications", cancellationAndNotifications);
    test("resumed tampering", resumedTampering); test("range fragmentation", fragmentation);
    test("concurrency/shutdown", concurrencyAndShutdown); test("schema/permissions", schemaAndPermissions); test("large sparse/bounds", largeSparseAndBounds);
    test("injected failures", injectedFailures); test("reduced limits", reducedLimits);
    test("missing pending/orphans", missingPendingAndOrphans);
    test("abandonment/hash cancellation", abandonedHandlesAndCancellation);
    test("race/permissions", raceAndPermissions); test("backpressure", backpressure); test("diagnostic redaction", diagnostics);
    test("crash recovery", crashRecovery); test("overwrite crash", overwriteCrash);
    return failures ? 1 : 0;
}
