#include <dsfilemanager/readonlyfilemanager.h>
#include <dsfilemanager/transferverifier.h>
#include <sqlite3.h>
#include <QThread>
#include <QProcess>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStorageInfo>
#include <sys/mount.h>
#include <thread>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <source_location>
#include <unistd.h>
#include <sys/stat.h>
using namespace dsfm;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #__VA_ARGS__); } while(false)
template<typename T> T get(QCoro::Task<Result<T>> task, std::source_location at=std::source_location::current()) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result) throw std::runtime_error(std::string(at.file_name())+":"+std::to_string(at.line())+" storage error=" + std::to_string(int(result.error().code)) + " native=" + std::to_string(result.error().native_code));
    return std::move(result).value();
}
void get(QCoro::Task<Result<void>> task) { auto result = QCoro::waitFor(std::move(task)); if (!result) throw std::runtime_error("storage error=" + std::to_string(int(result.error().code))); }
template<typename T> void error(QCoro::Task<Result<T>> task, ErrorCode expected) { auto result = QCoro::waitFor(std::move(task)); CHECK(!result); CHECK(result.error().code == expected); }
void write(QString path, QByteArray bytes) { QFile f(path); CHECK(f.open(QIODevice::WriteOnly)); CHECK(f.write(bytes) == bytes.size()); }
void settle(ReadOnlyFileManager& manager, NamespaceId ns, int maximum_rounds = 5000) {
    for (int i = 0; i < maximum_rounds; ++i) {
        auto info = get(manager.namespaceInfo(ns));
        if (info.readiness_error) throw std::runtime_error("scan error=" + std::to_string(int(info.readiness_error->code)) + " native=" + std::to_string(info.readiness_error->native_code));
        if (info.status == NamespaceStatus::Ready) return;
        QCoreApplication::processEvents();
    }
    throw std::runtime_error("scan did not settle");
}
void basic() {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root));
    write(root + "/hello", "hello world"); CHECK(QDir().mkdir(root + "/dir")); write(root + "/dir/child", "child");
    write(root + "/a\tb", "tab"); write(root + "/a%09b", "percent");
    const auto invalid_name = QFile::encodeName(root) + "/invalid-" + QByteArray(1, char(0xff));
    const auto raw_fd = ::open(invalid_name.constData(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0600); CHECK(raw_fd >= 0); CHECK(::write(raw_fd, "raw", 3) == 3); ::close(raw_fd);
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.common.limits.max_batch = 2;
    const auto catalog = temporary.path() + "/private";
    FileId id;
    {
        ReadOnlyFileManager manager(catalog); auto ns = get(manager.createSource(config));
        CHECK(ns.status == NamespaceStatus::Updating); settle(manager, config.common.id);
        ns = get(manager.namespaceInfo(config.common.id)); CHECK(!ns.capabilities.testFlag(Capability::Write));
        auto info = get(manager.lookup(config.common.id, "hello")); id = info.id; CHECK(info.hash_state == HashState::Missing);
        auto prepared = get(manager.prepareRead(config.common.id, id));
        CHECK(prepared.info.sha256 == QCryptographicHash::hash("hello world", QCryptographicHash::Sha256));
        CHECK(get(prepared.reader->readAt(0, 100)) == "hello world"); CHECK(get(prepared.reader->readAt(1000, 3)).isEmpty());
        error(prepared.reader->readAt(std::numeric_limits<quint64>::max(), 3), ErrorCode::InvalidArgument);
        get(prepared.reader->invalidate()); error(prepared.reader->readAt(0, 3), ErrorCode::Busy); get(prepared.reader->close());
        get(manager.reconcile(config.common.id, {})); settle(manager, config.common.id);
        CHECK(get(manager.lookup(config.common.id, "a%09b")).id != get(manager.lookup(config.common.id, "a%2509b")).id);
        CHECK(get(manager.lookup(config.common.id, "dir/child")).size == 5);
        auto raw = get(manager.openRead(config.common.id, get(manager.lookup(config.common.id, "invalid-%FF")).id)); CHECK(get(raw->readAt(0, 3)) == "raw"); get(raw->close());
        error(manager.mkdir(config.common.id, ns.root, "bad"), ErrorCode::UnsupportedCapability);
        auto epoch = std::make_shared<std::atomic<quint64>>(1);
        AccessContext context; context.generation = 1; context.current_generation = epoch;
        context.check = [id](NamespaceId, QUuid target, AccessOperation) -> Result<bool> { return target != id; };
        error(manager.stat(config.common.id, id, context), ErrorCode::MissingEntry);
        auto reader = get(manager.openRead(config.common.id, id)); write(root + "/hello", "changed");
        error(reader->readAt(0, 100), ErrorCode::Busy); get(reader->close());
        get(manager.reconcile(config.common.id, {})); settle(manager, config.common.id);
        CHECK(get(manager.lookup(config.common.id, "hello")).hash_state == HashState::Missing);
        get(manager.shutdown());
    }
    {
        ReadOnlyFileManager manager(catalog); get(manager.openSource(config)); settle(manager, config.common.id);
        CHECK(get(manager.lookup(config.common.id, "hello")).id == id); get(manager.shutdown());
    }
    CHECK(::unlink(invalid_name.constData()) == 0);
}
void rehashAndCancellation() {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root)); const QByteArray content(2 << 20, 'a'); write(root + "/file", content);
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.verification = ReadVerification::Rehash; config.common.limits.max_io_bytes = 32768;
    ReadOnlyFileManager manager(temporary.path() + "/private"); get(manager.createSource(config)); settle(manager, config.common.id);
    const auto file = get(manager.lookup(config.common.id, "file")); auto first = get(manager.prepareRead(config.common.id, file.id));
    CHECK(first.info.sha256 == QCryptographicHash::hash(content, QCryptographicHash::Sha256)); get(first.reader->close());
    CancellationToken cancel; std::optional<Result<PreparedRead>> outcome;
    auto preparation = manager.prepareRead(config.common.id, file.id, {{}, {}, cancel});
    preparation.then([&](Result<PreparedRead> value) { outcome.emplace(std::move(value)); });
    bool observed = false;
    for (int i = 0; i < 200 && !outcome; ++i) {
        const auto state = get(manager.stat(config.common.id, file.id));
        if (state.hash_state == HashState::Computing) { observed = true; cancel.cancel(); }
    }
    CHECK(observed);
    for (int i = 0; i < 200 && !outcome; ++i) { get(manager.namespaceInfo(config.common.id)); QCoreApplication::processEvents(); }
    CHECK(outcome && !*outcome && outcome->error().code == ErrorCode::Cancelled);
    CHECK(get(manager.stat(config.common.id, file.id)).hash_state == HashState::Missing);
    auto again = get(manager.prepareRead(config.common.id, file.id)); CHECK(again.info.sha256 == first.info.sha256); get(again.reader->close());
    get(manager.shutdown());
}
void exclusions() {
    QTemporaryDir temporary; auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root));
    write(root + "/public", "public"); write(root + "/secret", "secret");
    CHECK(::link(QFile::encodeName(root + "/secret").constData(), QFile::encodeName(root + "/alias").constData()) == 0);
    CHECK(::mkfifo(QFile::encodeName(root + "/pipe").constData(), 0600) == 0);
    CHECK(::symlink("secret", QFile::encodeName(root + "/link").constData()) == 0);
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.symlinks = SymlinkPolicy::Always;
    config.exclusions.objects = {root + "/secret", root + "/pipe"};
    ReadOnlyFileManager manager(temporary.path() + "/private"); auto ns = get(manager.createSource(config)); settle(manager, config.common.id);
    for (const auto& name : {"secret", "alias", "pipe", "link"}) error(manager.lookup(config.common.id, name), ErrorCode::MissingEntry);
    CHECK(get(manager.list(config.common.id, ns.root)).entries.size() == 1);
    auto info = get(manager.lookup(config.common.id, "public")); auto reader = get(manager.openRead(config.common.id, info.id));
    auto policy = config.exclusions; policy.generation = 2; policy.objects.append(root + "/public");
    get(manager.updateExclusions(config.common.id, policy)); error(reader->readAt(0, 10), ErrorCode::MissingEntry);
    error(manager.stat(config.common.id, info.id), ErrorCode::MissingEntry); get(manager.shutdown());
}
void snapshotsAndMoves() {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root));
    CHECK(QDir().mkdir(root + "/directory")); write(root + "/directory/file", "original");
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root};
    config.verification = ReadVerification::Snapshot;
    const auto private_root = temporary.path() + "/private";
    QByteArray lease, hash; FileId file_id, directory_id;
    {
        ReadOnlyFileManager manager(private_root); get(manager.createSource(config)); settle(manager, config.common.id);
        auto file = get(manager.lookup(config.common.id, "directory/file")); file_id = file.id;
        directory_id = get(manager.lookup(config.common.id, "directory")).id;
        auto prepared = get(manager.prepareRead(config.common.id, file.id));
        CHECK(prepared.consistency == ReadConsistency::Snapshot); CHECK(!prepared.resume_lease.isEmpty());
        lease = prepared.resume_lease; hash = prepared.info.sha256;
        write(root + "/directory/file", "modified-long");
        CHECK(get(prepared.reader->readAt(0, 100)) == "original"); get(prepared.reader->close());
        CHECK(QDir().rename(root + "/directory", root + "/renamed"));
        get(manager.reconcile(config.common.id, {})); settle(manager, config.common.id);
        CHECK(get(manager.lookup(config.common.id, "renamed")).id == directory_id);
        CHECK(get(manager.lookup(config.common.id, "renamed/file")).id == file_id);
        get(manager.shutdown());
    }
    {
        ReadOnlyFileManager manager(private_root); get(manager.openSource(config)); settle(manager, config.common.id);
        ReadPreparationOptions options; options.resume_lease = lease; options.expected_sha256 = hash;
        auto prepared = get(manager.prepareRead(config.common.id, file_id, options));
        CHECK(get(prepared.reader->readAt(0, 100)) == "original"); get(prepared.reader->close());
        options.expected_sha256 = QByteArray(32, 'x'); error(manager.prepareRead(config.common.id, file_id, options), ErrorCode::MissingEntry);
        get(manager.shutdown());
    }
}
void aliasesAndAuthorization() {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source", destination = temporary.path() + "/destination";
    CHECK(QDir().mkdir(root)); CHECK(QDir().mkdir(destination)); write(destination + "/file", "target"); write(destination + "/hidden", "classified");
    write(root + "/public", "public"); write(root + "/second", "second");
    CHECK(::symlink("../destination", QFile::encodeName(root + "/directory-link").constData()) == 0);
    CHECK(::symlink("../destination/file", QFile::encodeName(root + "/file-link").constData()) == 0);
    CHECK(::symlink(".", QFile::encodeName(root + "/cycle").constData()) == 0);
    ReadOnlyFileManager manager(temporary.path() + "/private");
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root};
    auto never = get(manager.createSource(config)); settle(manager, config.common.id);
    error(manager.lookup(config.common.id, "directory-link"), ErrorCode::MissingEntry);
    config.common.id = QUuid::createUuid(); config.symlinks = SymlinkPolicy::Always;
    auto always = get(manager.createSource(config)); settle(manager, config.common.id);
    AccessContext external; external.allow_external_files=true;external.assignment_directory=always.root;
    error(manager.lookup(config.common.id,"directory-link",external),ErrorCode::MissingEntry);
    error(manager.lookup(config.common.id,"directory-link/file",external),ErrorCode::MissingEntry);
    error(manager.lookup(config.common.id,"file-link"),ErrorCode::MissingEntry);
    auto outside=get(manager.lookup(config.common.id,"file-link",external));
    auto reader=get(manager.openRead(config.common.id,outside.id,external));CHECK(get(reader->readAt(0,100))=="target");get(reader->close());
    auto visible=get(manager.list(config.common.id,always.root,{},external));
    for (const auto& entry:visible.entries) CHECK(entry.name!="directory-link" && entry.name!="cycle");
    config.common.id = QUuid::createUuid(); config.symlinks = SymlinkPolicy::WithinVfs; config.exclusions.objects = {destination + "/hidden"};
    auto within = get(manager.createSource(config)); settle(manager, config.common.id); const auto source_ns = config.common.id;
    ReadOnlyNamespaceConfig target_config; target_config.common = {QUuid::createUuid(), destination}; target_config.verification = ReadVerification::Snapshot;
    auto target_ns = get(manager.createSource(target_config)); settle(manager, target_config.common.id);
    const auto file = get(manager.lookup(target_config.common.id, "file"));
    struct stat directory_stat{}, file_stat{}; CHECK(::stat(QFile::encodeName(destination).constData(), &directory_stat) == 0);
    CHECK(::stat(QFile::encodeName(destination + "/file").constData(), &file_stat) == 0);
    auto generation = std::make_shared<std::atomic<quint64>>(1);
    AccessContext allowed; allowed.authorization_scope = QUuid::createUuid(); allowed.generation = 1; allowed.current_generation = generation;
    allowed.check = [](NamespaceId, QUuid, AccessOperation) -> Result<bool> { return true; };
    allowed.resolve_target = [=](FilesystemIdentity identity) -> Result<EntryTarget> {
        if (identity.device == quint64(directory_stat.st_dev) && identity.inode == quint64(directory_stat.st_ino)) return EntryTarget{target_config.common.id, target_ns.root};
        if (identity.device == quint64(file_stat.st_dev) && identity.inode == quint64(file_stat.st_ino)) return EntryTarget{target_config.common.id, file.id};
        return Error{ErrorCode::MissingEntry};
    };
    auto denied = allowed; denied.authorization_scope = QUuid::createUuid();
    denied.check = [file, target_ns](NamespaceId, QUuid id, AccessOperation) -> Result<bool> { return id != file.id && id != target_ns.root; };
    error(manager.lookup(source_ns, "file-link"), ErrorCode::MissingEntry);
    error(manager.lookup(source_ns, "file-link", denied), ErrorCode::MissingEntry);
    const auto alias = get(manager.lookup(source_ns, "file-link", allowed));
    auto prepared = get(manager.prepareRead(source_ns, alias.id, {}, allowed)); CHECK(prepared.consistency == ReadConsistency::Snapshot); CHECK(get(prepared.reader->readAt(0, 100)) == "target");
    CHECK(get(manager.lookup(source_ns, "directory-link/file", allowed)).id == file.id);
    const auto directory = get(manager.lookup(source_ns, "directory-link", allowed));
    auto directory_page = get(manager.list(source_ns, directory.id, {}, allowed)); CHECK(directory_page.entries.size() == 1); CHECK(directory_page.entries.front().id == file.id); CHECK(directory_page.entries.front().namespace_id == target_config.common.id);
    error(manager.lookup(source_ns, "directory-link/hidden", allowed), ErrorCode::MissingEntry);
    auto page = get(manager.list(source_ns, within.root, {1, {}}, allowed)); CHECK(!page.next_cursor.isEmpty());
    error(manager.list(source_ns, within.root, {1, page.next_cursor}, denied), ErrorCode::StaleCursor);
    auto hidden = get(manager.list(source_ns, within.root, {}, denied));
    for (const auto& entry : hidden.entries) CHECK(entry.name != "directory-link" && entry.name != "file-link" && entry.name != "cycle");
    page = get(manager.list(source_ns, within.root, {1, {}}, allowed)); generation->store(2);
    error(manager.list(source_ns, within.root, {1, page.next_cursor}, allowed), ErrorCode::StaleCursor);
    error(prepared.reader->readAt(0, 100), ErrorCode::MissingEntry); get(prepared.reader->close()); get(manager.shutdown());
}
void nativeAliasComposition() {
    QTemporaryDir temporary; auto native = std::make_shared<NativeFileManager>();
    NamespaceConfig native_config{QUuid::createUuid(), temporary.path() + "/native", PhysicalNaming::OriginalName};
    auto native_ns = get(native->createNamespace(native_config));
    auto private_dir = get(native->mkdir(native_config.id, native_ns.root, "private"));
    auto publish = [&](DirectoryId parent, QString name, QByteArray bytes) {
        auto pending = get(native->createFile(native_config.id, {parent, name, ConflictPolicy::Fail, {quint64(bytes.size()), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)}})); get(pending.writer->writeAt(0, bytes)); get(pending.writer->commit()); return pending.info.file_id;
    };
    const auto public_id = publish(native_ns.root, "public", "public"); publish(private_dir, "secret", "secret");
    const auto source = temporary.path() + "/source"; CHECK(QDir().mkdir(source));
    CHECK(::symlink(QFile::encodeName(native_config.storage_root).constData(), QFile::encodeName(source + "/directory").constData()) == 0);
    CHECK(::symlink(QFile::encodeName(native_config.storage_root + "/public").constData(), QFile::encodeName(source + "/file").constData()) == 0);
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), source}; config.symlinks = SymlinkPolicy::WithinVfs;
    config.exclusions.directory_roots = {native_config.storage_root + "/.filemanager", native_config.storage_root + "/private"};
    ReadOnlyFileManager manager(temporary.path() + "/index"); get(manager.createSource(config)); settle(manager, config.common.id);
    struct stat root_stat{}, file_stat{}; CHECK(::stat(QFile::encodeName(native_config.storage_root).constData(), &root_stat) == 0);
    CHECK(::stat(QFile::encodeName(native_config.storage_root + "/public").constData(), &file_stat) == 0);
    AccessContext access; access.authorization_scope = QUuid::createUuid(); access.current_generation = std::make_shared<std::atomic<quint64>>(1); access.generation = 1;
    access.check = [](NamespaceId, QUuid, AccessOperation) -> Result<bool> { return true; };
    access.resolve_target = [=](FilesystemIdentity identity) -> Result<EntryTarget> {
        if (identity.device == quint64(root_stat.st_dev) && identity.inode == quint64(root_stat.st_ino)) return EntryTarget{native_config.id, native_ns.root};
        if (identity.device == quint64(file_stat.st_dev) && identity.inode == quint64(file_stat.st_ino)) return EntryTarget{native_config.id, public_id};
        return Error{ErrorCode::MissingEntry};
    };
    std::weak_ptr<NativeFileManager> weak = native;
    access.backend = [weak, id = native_config.id](NamespaceId ns) -> std::shared_ptr<IFileManager> { return ns == id ? weak.lock() : nullptr; };
    const auto directory = get(manager.lookup(config.common.id, "directory", access));
    const auto page = get(manager.list(config.common.id, directory.id, {}, access)); CHECK(page.entries.size() == 1); CHECK(page.entries.front().name == "public"); CHECK(page.entries.front().namespace_id == native_config.id);
    error(manager.lookup(config.common.id, "directory/private/secret", access), ErrorCode::MissingEntry);
    CHECK(get(manager.lookup(config.common.id, "directory/public", access)).id == public_id);
    const auto file = get(manager.lookup(config.common.id, "file", access)); auto prepared = get(manager.prepareRead(config.common.id, file.id, {}, access));
    const auto queued_offer_guard = prepared.reader->accessGuard(); CHECK(queued_offer_guard());
    CHECK(get(prepared.reader->readAt(0, 64)) == "public"); get(prepared.reader->close());
    auto snapshot_config = config; snapshot_config.common.id = QUuid::createUuid(); snapshot_config.verification = ReadVerification::Snapshot;
    get(manager.createSource(snapshot_config)); settle(manager, snapshot_config.common.id);
    const auto snapshot_alias = get(manager.lookup(snapshot_config.common.id, "file", access));
    auto snapshot = get(manager.prepareRead(snapshot_config.common.id, snapshot_alias.id, {}, access));
    CHECK(snapshot.consistency == ReadConsistency::Snapshot && !snapshot.resume_lease.isEmpty()); CHECK(snapshot.info.namespace_id == snapshot_config.common.id);
    CHECK(get(snapshot.reader->readAt(0, 64)) == "public"); get(snapshot.reader->close());
    ReadPreparationOptions resume; resume.resume_lease = snapshot.resume_lease; resume.expected_sha256 = snapshot.info.sha256; resume.expected_generation = snapshot.info.generation;
    auto resumed = get(manager.prepareRead(snapshot_config.common.id, snapshot_alias.id, resume, access)); CHECK(get(resumed.reader->readAt(0, 64)) == "public"); get(resumed.reader->close());
    auto first_page = get(manager.list(config.common.id, directory.id, {1, {}}, access)); CHECK(!first_page.next_cursor.isEmpty());
    auto policy = config.exclusions; policy.generation = 2; policy.objects.append(native_config.storage_root + "/public");
    get(manager.updateExclusions(config.common.id, policy));
    CHECK(!queued_offer_guard());
    error(manager.list(config.common.id, directory.id, {1, first_page.next_cursor}, access), ErrorCode::StaleCursor);
    get(manager.shutdown()); get(native->shutdown());
}
void verifier() {
    const auto hash = QCryptographicHash::hash("abcdef", QCryptographicHash::Sha256);
    TransferVerifier good(6, hash); get(good.add(0, "abc")); get(good.add(3, "def")); get(good.finish());
    error(good.add(6, {}), ErrorCode::IntegrityError);
    TransferVerifier bad(6, hash); get(bad.add(0, "abc")); get(bad.add(3, "xyz")); error(bad.finish(), ErrorCode::IntegrityError); error(bad.finish(), ErrorCode::IntegrityError);
    TransferVerifier suffix(6, hash); error(suffix.add(3, "def"), ErrorCode::IntegrityError); error(suffix.finish(), ErrorCode::IntegrityError);
    TransferVerifier duplicate(6, hash); get(duplicate.add(0, "abc")); error(duplicate.add(0, "abc"), ErrorCode::IntegrityError);
    TransferVerifier short_file(6, hash); get(short_file.add(0, "abc")); error(short_file.finish(), ErrorCode::IntegrityError);
    TransferVerifier empty(0, QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256)); get(empty.finish());
    TransferVerifier cancelled(6, hash); CancellationToken token; token.cancel(); error(cancelled.add(0, "abc", token), ErrorCode::Cancelled); error(cancelled.finish(), ErrorCode::IntegrityError);
}
void subscriptionsAndQuotas() {
    QTemporaryDir temporary; auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root));
    CHECK(QDir().mkdir(root + "/a")); CHECK(QDir().mkdir(root + "/b")); write(root + "/file", "abcdef");
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.common.limits.max_notifications = 2; config.limits.max_cached_directories = 1;
    config.common.limits.max_io_bytes = 2; config.common.limits.max_batch = 2; config.common.limits.max_operations = 1;
    ReadOnlyFileManager manager(temporary.path() + "/private"); auto ns = get(manager.createSource(config)); settle(manager, config.common.id);
    QObject context; int notices = 0;
    auto a = get(manager.lookup(config.common.id, "a")); auto b = get(manager.lookup(config.common.id, "b"));
    auto pin = get(manager.subscribeDirectory(config.common.id, a.id, {&context, [&](DirectoryChange) { ++notices; }}));
    error(manager.subscribeDirectory(config.common.id, b.id, {&context, [](DirectoryChange) {}}), ErrorCode::QuotaExceeded);
    auto moved = std::move(pin); CHECK(!pin); moved.reset();
    auto other = get(manager.subscribeDirectory(config.common.id, b.id, {&context, [](DirectoryChange) {}})); other.reset();
    const auto file = get(manager.lookup(config.common.id, "file"));
    CancellationToken token; token.cancel(); error(manager.prepareRead(config.common.id, file.id, {{}, {}, token}), ErrorCode::Cancelled);
    auto legacy = manager.subscribe(config.common.id, &context, [&](Change) { ++notices; }); CHECK(legacy);
    get(manager.reconcile(config.common.id, {})); settle(manager, config.common.id); QCoreApplication::processEvents(); CHECK(notices > 0);
    manager.unsubscribe(legacy.value());
    get(manager.shutdown());
}
void schemaAndPartialScan() {
    QTemporaryDir temporary; auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root));
    write(root + "/first", "first"); write(root + "/second", "second");
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.common.limits.max_entries = 4; config.common.limits.max_batch = 1;
    const auto private_root = temporary.path() + "/private"; DirectoryId root_id;
    {
        ReadOnlyFileManager manager(private_root); root_id = get(manager.createSource(config)).root; settle(manager, config.common.id);
        write(root + "/third", "third"); write(root + "/fourth", "fourth");
        get(manager.reconcile(config.common.id, {}));
        bool paused = false;
        for (int i = 0; i < 1000; ++i) {
            const auto state = get(manager.namespaceInfo(config.common.id));
            if (state.readiness_error) { CHECK(state.readiness_error->code == ErrorCode::QuotaExceeded); paused = true; break; }
            QCoreApplication::processEvents();
        }
        CHECK(paused);
        CHECK(get(manager.lookup(config.common.id, "first")).status != EntryStatus::Missing);
        CHECK(get(manager.lookup(config.common.id, "second")).status != EntryStatus::Missing);
        get(manager.shutdown());
    }
    const auto path = private_root + '/' + config.common.id.toString(QUuid::WithoutBraces) + "/catalog.db";
    sqlite3* database = nullptr; CHECK(sqlite3_open(QFile::encodeName(path).constData(), &database) == SQLITE_OK);
    CHECK(sqlite3_exec(database, "UPDATE meta SET value='future-unsupported' WHERE key='schema'", nullptr, nullptr, nullptr) == SQLITE_OK); sqlite3_close(database);
    {
        ReadOnlyFileManager manager(private_root); error(manager.openSource(config), ErrorCode::UnsupportedCapability);
        CHECK(QFile::remove(root + "/third")); CHECK(QFile::remove(root + "/fourth"));
        auto rebuilt = get(manager.reindexSource(config)); CHECK(rebuilt.root != root_id); settle(manager, config.common.id);
        CHECK(QFileInfo::exists(private_root + '/' + config.common.id.toString(QUuid::WithoutBraces) + ".reindex-backup/catalog.db"));
        CHECK(get(manager.lookup(config.common.id, "first")).size == 5); get(manager.shutdown());
    }
}
void snapshotBudgetsAndExpiration() {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root)); write(root + "/file", "abcdef");
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.verification = ReadVerification::Snapshot;
    config.limits.reader_lifetime_ms = 500; config.common.limits.max_file_size = 16; config.common.limits.max_staging_bytes = 16;
    config.common.limits.max_pending = 2;
    ReadOnlyFileManager manager(temporary.path() + "/private"); get(manager.createSource(config)); settle(manager, config.common.id);
    const auto file = get(manager.lookup(config.common.id, "file"));
    auto a = get(manager.prepareRead(config.common.id, file.id)); get(a.reader->close());
    auto b = get(manager.prepareRead(config.common.id, file.id));
    error(manager.prepareRead(config.common.id, file.id), ErrorCode::QuotaExceeded);
    QEventLoop loop; QTimer::singleShot(600, &loop, &QEventLoop::quit); loop.exec();
    error(b.reader->readAt(0, 6), ErrorCode::Cancelled);
    auto c = get(manager.prepareRead(config.common.id, file.id)); CHECK(get(c.reader->readAt(0, 6)) == "abcdef");
    ReadPreparationOptions options; options.resume_lease = a.resume_lease; options.expected_sha256 = a.info.sha256;
    error(manager.prepareRead(config.common.id, file.id, options), ErrorCode::MissingEntry); get(manager.shutdown());
}
void stagedSpecialFileReplacement() {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root)); write(root + "/file", "public");
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.verification = ReadVerification::Snapshot;
    const auto private_root = temporary.path() + "/private"; ReadOnlyFileManager manager(private_root);
    get(manager.createSource(config)); settle(manager, config.common.id);
    auto file = get(manager.lookup(config.common.id, "file")); auto prepared = get(manager.prepareRead(config.common.id, file.id)); get(prepared.reader->close());
    const auto staged = QFile::encodeName(private_root + '/' + config.common.id.toString(QUuid::WithoutBraces) + "/snapshot-") + prepared.resume_lease;
    CHECK(::unlink(staged.constData()) == 0); CHECK(::mkfifo(staged.constData(), 0600) == 0);
    ReadPreparationOptions options; options.resume_lease = prepared.resume_lease; options.expected_sha256 = prepared.info.sha256;
    error(manager.prepareRead(config.common.id, file.id, options), ErrorCode::ExternalModification);
    CHECK(::unlink(staged.constData()) == 0); get(manager.shutdown());
}
void pagingAndResponsiveness(int entry_count = 1200) {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source"; CHECK(QDir().mkdir(root));
    for (int i = 0; i < entry_count; ++i) {
        write(root + QStringLiteral("/file-%1").arg(i, 4, 10, QChar('0')), "content");
        if (entry_count >= 1000000 && (i + 1) % 250000 == 0) std::cout << "Created " << i + 1 << " source files" << std::endl;
    }
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.common.limits.max_entries = quint32(entry_count + 100);
    config.common.limits.max_catalog_bytes = entry_count >= 1000000 ? (1ULL << 30) : (64ULL << 20);
    config.common.limits.max_batch = entry_count >= 1000000 ? 256 : 17; config.common.limits.max_page_size = entry_count >= 1000000 ? 1000 : 31;
    ReadOnlyFileManager manager(temporary.path() + "/private"); int heartbeats = 0;
    QTimer heartbeat; QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; }); heartbeat.start(1);
    auto ns = get(manager.createSource(config)); settle(manager, config.common.id, entry_count >= 1000000 ? 150000 : 5000); CHECK(heartbeats > 0);
    if (entry_count >= 1000000) std::cout << "Indexed one million source files; verifying pages" << std::endl;
    PageRequest page{config.common.limits.max_page_size, {}}; QString previous; int count = 0; bool reconciled = false;
    do {
        auto children = get(manager.list(config.common.id, ns.root, page));
        for (const auto& file : children.entries) { CHECK(previous.isEmpty() || previous < file.name); previous = file.name; ++count; }
        if (entry_count >= 1000000 && count % 100096 == 0) std::cout << "Verified " << count << " entries" << std::endl;
        page.cursor = children.next_cursor;
        if (!reconciled && !page.cursor.isEmpty() && entry_count < 1000000) {
            const auto before = get(manager.namespaceInfo(config.common.id)).generation;
            get(manager.reconcile(config.common.id, {})); settle(manager, config.common.id);
            CHECK(get(manager.namespaceInfo(config.common.id)).generation == before); reconciled = true;
        }
    } while (!page.cursor.isEmpty());
    CHECK(count == entry_count); get(manager.shutdown());
    QFile fixture(QStringLiteral(DSFM_TEST_SOURCE_DIR) + "/fixtures/readonly-schema-v1.json"); CHECK(fixture.open(QIODevice::ReadOnly));
    const auto schema = QJsonDocument::fromJson(fixture.readAll()).object(); CHECK(schema["name_encoding"].toInt() == int(linux_name_encoding_version));
    const auto path = temporary.path() + "/private/" + config.common.id.toString(QUuid::WithoutBraces) + "/catalog.db";
    sqlite3* database = nullptr; CHECK(sqlite3_open(QFile::encodeName(path).constData(), &database) == SQLITE_OK);
    sqlite3_stmt* query = nullptr; CHECK(sqlite3_prepare_v2(database, "SELECT value FROM meta WHERE key='schema'", -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW); CHECK(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query, 0))) == schema["schema"].toString()); sqlite3_finalize(query);
    const auto tables = schema["tables"].toObject();
    for (auto it = tables.begin(); it != tables.end(); ++it) {
        const auto sql = QStringLiteral("PRAGMA table_info(%1)").arg(it.key()).toUtf8();
        CHECK(sqlite3_prepare_v2(database, sql.constData(), -1, &query, nullptr) == SQLITE_OK);
        QJsonArray columns;
        while (sqlite3_step(query) == SQLITE_ROW) columns.append(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query, 1))));
        CHECK(columns == it.value().toArray()); sqlite3_finalize(query);
    }
    const auto indexes = schema["indexes"].toObject();
    for (auto it = indexes.begin(); it != indexes.end(); ++it) {
        const auto sql = QStringLiteral("PRAGMA index_info(%1)").arg(it.key()).toUtf8();
        CHECK(sqlite3_prepare_v2(database, sql.constData(), -1, &query, nullptr) == SQLITE_OK);
        QJsonArray columns;
        while (sqlite3_step(query) == SQLITE_ROW) columns.append(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query, 2))));
        CHECK(columns == it.value().toArray()); sqlite3_finalize(query);
    }
    sqlite3_close(database);
}
void mountAliases() {
    QTemporaryDir temporary; auto root = temporary.path() + "/source", excluded = temporary.path() + "/excluded";
    CHECK(QDir().mkdir(root)); CHECK(QDir().mkdir(excluded)); CHECK(QDir().mkdir(excluded + "/child")); CHECK(QDir().mkdir(root + "/mounted"));
    write(excluded + "/child/secret", "secret"); write(root + "/public", "public");
    CHECK(::mount(QFile::encodeName(excluded + "/child").constData(), QFile::encodeName(root + "/mounted").constData(), nullptr, MS_BIND, nullptr) == 0);
    struct Unmount { QByteArray path; ~Unmount() { ::umount2(path.constData(), MNT_DETACH); } } unmount{QFile::encodeName(root + "/mounted")};
    ReadOnlyFileManager manager(temporary.path() + "/private"); ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root};
    config.symlinks = SymlinkPolicy::Always; config.exclusions.directory_roots = {excluded};
    get(manager.createSource(config)); settle(manager, config.common.id);
    error(manager.lookup(config.common.id, "mounted"), ErrorCode::MissingEntry);
    error(manager.lookup(config.common.id, "mounted/secret"), ErrorCode::MissingEntry); get(manager.shutdown());
}
void optionalMountAliases() {
    QProcess probe; probe.start("unshare", {"--user", "--map-root-user", "--mount", "--fork", "true"});
    if (!probe.waitForStarted(1000)) { std::cout << "mount aliases skipped: unshare unavailable\n"; return; }
    CHECK(probe.waitForFinished(5000));
    if (probe.exitCode() != 0) { std::cout << "mount aliases skipped: private mount namespaces unavailable\n"; return; }
    QProcess child; child.start("unshare", {"--user", "--map-root-user", "--mount", "--fork", QCoreApplication::applicationFilePath(), "--mount-child"});
    CHECK(child.waitForStarted(1000)); CHECK(child.waitForFinished(30000));
    if (child.exitCode()) throw std::runtime_error(child.readAllStandardError().toStdString());
}
void symlinkReplacementRace() {
    QTemporaryDir temporary; const auto root = temporary.path() + "/source", private_dir = temporary.path() + "/excluded";
    CHECK(QDir().mkdir(root)); CHECK(QDir().mkdir(private_dir)); write(temporary.path() + "/public", "public"); write(private_dir + "/secret", "secret");
    const auto link = QFile::encodeName(root + "/link"), replacement = QFile::encodeName(root + "/replacement");
    CHECK(::symlink("../public", link.constData()) == 0);
    ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), root}; config.symlinks = SymlinkPolicy::Always; config.exclusions.directory_roots = {private_dir};
    ReadOnlyFileManager manager(temporary.path() + "/private"); get(manager.createSource(config)); settle(manager, config.common.id);
    AccessContext access;access.allow_external_files=true;
    auto id = get(manager.lookup(config.common.id, "link",access)).id; auto reader = get(manager.openRead(config.common.id, id,access));
    std::jthread mutate([&] {
        for (int i = 0; i < 200; ++i) {
            const auto target = i % 2 ? QByteArray("../public") : QFile::encodeName(private_dir + "/secret");
            if (::symlink(target.constData(), replacement.constData()) == 0) ::rename(replacement.constData(), link.constData());
        }
    });
    for (int i = 0; i < 50; ++i) {
        auto bytes = QCoro::waitFor(reader->readAt(0, 6));
        if (bytes) CHECK(bytes.value() == "public");
        else CHECK(bytes.error().code == ErrorCode::MissingEntry || bytes.error().code == ErrorCode::Busy || bytes.error().code == ErrorCode::ExternalModification);
    }
    mutate.join(); get(reader->close()); get(manager.shutdown());
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().contains("--million-entry-paging")) {
            if (qEnvironmentVariable("DSFM_RUN_MILLION_ENTRY_TEST") != "1" || QStorageInfo(QDir::tempPath()).bytesAvailable() < (6LL << 30)) {
                std::cout << "million-entry test requires DSFM_RUN_MILLION_ENTRY_TEST=1 and 6 GiB free temporary storage\n"; return 77;
            }
            pagingAndResponsiveness(1000000); return 0;
        }
        if (app.arguments().contains("--mount-child")) { mountAliases(); return 0; } if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "basic\n"; basic();
        rehashAndCancellation();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "exclusions\n"; exclusions();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "snapshotsAndMoves\n"; snapshotsAndMoves();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "aliasesAndAuthorization\n"; aliasesAndAuthorization();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "nativeAliasComposition\n"; nativeAliasComposition();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "verifier\n"; verifier();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "subscriptionsAndQuotas\n"; subscriptionsAndQuotas();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "schemaAndPartialScan\n"; schemaAndPartialScan();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "snapshotBudgetsAndExpiration\n"; snapshotBudgetsAndExpiration(); stagedSpecialFileReplacement();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "pagingAndResponsiveness\n"; pagingAndResponsiveness();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "optionalMountAliases\n"; optionalMountAliases();
        if (qEnvironmentVariableIsSet("DSFM_TEST_VERBOSE")) std::cerr << "symlinkReplacementRace\n"; symlinkReplacementRace();
         std::cout << "read-only tests passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
