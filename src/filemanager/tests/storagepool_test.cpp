#include <dsfilemanager/storagepool.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <unistd.h>
using namespace dsfm;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__))                                                                                            \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #__VA_ARGS__);       \
    } while (false)
template <class T> T get(QCoro::Task<Result<T>> task, std::source_location at = std::source_location::current()) {
    auto r = QCoro::waitFor(std::move(task));
    if (!r)
        throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                                 " error=" + std::to_string(int(r.error().code)));
    return std::move(r).value();
}
void get(QCoro::Task<Result<void>> task, std::source_location at = std::source_location::current()) {
    auto r = QCoro::waitFor(std::move(task));
    if (!r)
        throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                                 " error=" + std::to_string(int(r.error().code)));
}
template <class T> void error(QCoro::Task<Result<T>> task, ErrorCode expected) {
    auto r = QCoro::waitFor(std::move(task));
    CHECK(!r);
    CHECK(r.error().code == expected);
}
StoragePoolLimits limits() {
    StoragePoolLimits l;
    l.minimum_available_bytes = 0;
    l.minimum_available_percent = 0;
    l.recovery_headroom = 0;
    l.maximum_owned_bytes = storage_pool_journal_reservation + 1024;
    for (auto &c : l.classes)
        c = {1024, 4, 4};
    l.classes[0].bytes = storage_pool_journal_reservation + 1024;
    l.classes[2].bytes = 128;
    return l;
}
void admissionAndRecovery() {
    QTemporaryDir temp;
    CHECK(QDir().mkdir(temp.path() + "/a"));
    CHECK(QDir().mkdir(temp.path() + "/b"));
    auto ns = QUuid::createUuid(), other = QUuid::createUuid(), owner = QUuid::createUuid();
    CHECK(QFile::setPermissions(temp.path() + "/a",
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    CHECK(QFile::setPermissions(temp.path() + "/b",
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    auto path = temp.path() + "/journal";
    StoragePool pool(path, limits());
    get(pool.open());
    auto id = get(pool.registerNamespace(ns, temp.path() + "/a"));
    CHECK(get(pool.registerNamespace(other, temp.path() + "/b")) == id);
    StorageReservation r{QUuid::createUuid(), id, ns, owner, QUuid::createUuid(), StorageClass::Incoming, 700, 0};
    get(pool.reserve(r));
    CHECK(get(pool.reserve(r)).id == r.id);
    auto second = r;
    second.id = QUuid::createUuid();
    second.namespace_id = other;
    second.reserved_total = 400;
    error(pool.reserve(second), ErrorCode::QuotaExceeded);
    error(pool.resize(r.id, QUuid::createUuid(), 800), ErrorCode::MissingEntry);
    get(pool.materialize(r.id, owner, 512));
    auto usage = get(pool.usage(id));
    CHECK(usage.materialized_physical >= 512 &&
          usage.materialized_physical + usage.outstanding == storage_pool_journal_reservation + 700);
    get(pool.resize(r.id, owner, 600));
    CHECK(get(pool.usage(id)).materialized_physical + get(pool.usage(id)).outstanding ==
          storage_pool_journal_reservation + 600);
    error(pool.resize(r.id, owner, 500), ErrorCode::InvalidArgument);
    second.reserved_total = 400;
    get(pool.reserve(second));
    CHECK(get(pool.usage(id)).reservations == 3);
    auto temporary = r;
    temporary.id = QUuid::createUuid();
    temporary.storage_class = StorageClass::Temporary;
    temporary.reserved_total = 129;
    temporary.materialized_physical = 0;
    error(pool.reserve(temporary), ErrorCode::QuotaExceeded);
    error(pool.reclassify(r.id, owner, StorageClass::Temporary), ErrorCode::QuotaExceeded);
    get(pool.reclassify(r.id, owner, StorageClass::Permanent));
    auto rows = get(pool.reservations(ns, 1));
    CHECK(rows.size() == 1 && rows.front().id == r.id);
    CHECK(get(pool.reservations(ns, 1, r.id)).isEmpty());
    StoragePool same_authority(path,limits());
    error(same_authority.open(),ErrorCode::NamespaceBusy);get(same_authority.shutdown());
    StoragePool independent(temp.path()+"/other-profile",limits());
    get(independent.open());
    auto independent_pool=get(independent.registerNamespace(QUuid::createUuid(),temp.path()+"/b"));
    CHECK(independent_pool!=id);
    get(independent.shutdown());
    auto reduced = limits();
    reduced.maximum_owned_bytes = storage_pool_journal_reservation + 500;
    get(pool.setLimits(id, reduced));
    get(pool.release(second.id, owner));
    get(pool.release(second.id, owner));
    CHECK(get(pool.usage(id)).materialized_physical >= 512);
    get(pool.shutdown());
    StoragePool reopened(path, limits());
    get(reopened.open());
    CHECK(get(reopened.registerNamespace(ns, temp.path() + "/a")) == id);
    CHECK(get(reopened.reservations(ns)).size() == 1);
    CHECK(get(reopened.usage(id)).materialized_physical + get(reopened.usage(id)).outstanding ==
          storage_pool_journal_reservation + 600);
    // A lowered ceiling prevents growth, but never prevents reclamation.
    error(reopened.resize(r.id, owner, 601), ErrorCode::QuotaExceeded);
    get(reopened.materialize(r.id, owner, 0));
    get(reopened.release(r.id, owner));
    CHECK(get(reopened.usage(id)).reservations == 1);
    get(reopened.shutdown());
}
void rootReplacement() {
    QTemporaryDir temp;
    auto root = temp.path() + "/root";
    CHECK(QDir().mkdir(root));
    CHECK(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    auto ns = QUuid::createUuid(), owner = QUuid::createUuid();
    StoragePool pool(temp.path() + "/pool", limits());
    get(pool.open());
    auto id = get(pool.registerNamespace(ns, root));
    CHECK(QDir().rename(root, temp.path() + "/moved"));
    CHECK(QDir().mkdir(root));
    CHECK(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    error(pool.reserve({{}, id, ns, owner, QUuid::createUuid(), StorageClass::Incoming, 1, 0}),
          ErrorCode::ExternalModification);
    get(pool.shutdown());
}
void concurrentAdmissions() {
    QTemporaryDir temp;
    CHECK(QDir().mkdir(temp.path() + "/root"));
    CHECK(QFile::setPermissions(temp.path() + "/root",
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    StoragePool pool(temp.path() + "/pool", limits());
    get(pool.open());
    auto ns = QUuid::createUuid(), owner = QUuid::createUuid();
    auto id = get(pool.registerNamespace(ns, temp.path() + "/root"));
    StorageReservation a{{}, id, ns, owner, QUuid::createUuid(), StorageClass::Incoming, 600, 0}, b = a;
    b.workflow = QUuid::createUuid();
    auto first = pool.reserve(a), second = pool.reserve(b);
    auto x = QCoro::waitFor(std::move(first)), y = QCoro::waitFor(std::move(second));
    CHECK(bool(x) != bool(y));
    CHECK((x ? y.error() : x.error()).code == ErrorCode::QuotaExceeded);
    get(pool.shutdown());
}
void crashRecovery() {
    QTemporaryDir temp;
    CHECK(QDir().mkdir(temp.path() + "/root"));
    CHECK(QFile::setPermissions(temp.path() + "/root",
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    auto ns = QUuid::createUuid(), owner = QUuid::createUuid();
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--crash", temp.path(), ns.toString(), owner.toString()});
    CHECK(child.waitForStarted(1000));
    CHECK(child.waitForFinished(10000));
    CHECK(child.exitCode() == 17);
    StoragePool pool(temp.path() + "/pool", limits());
    get(pool.open());
    auto id = get(pool.registerNamespace(ns, temp.path() + "/root"));
    auto retained = get(pool.reservations(ns));
    CHECK(retained.size() == 1 && retained[0].reserved_total == 700);
    CHECK(get(pool.usage(id)).materialized_physical + get(pool.usage(id)).outstanding == storage_pool_journal_reservation + 700);
    get(pool.release(retained[0].id, owner));
    get(pool.shutdown());
}
void schemaFixture() {
    QTemporaryDir temp;
    StoragePool pool(temp.path() + "/pool");
    get(pool.open());
    get(pool.shutdown());
    QFile fixture(QStringLiteral(DSFM_TEST_SOURCE_DIR) + "/fixtures/storage-pool-v1.json");
    CHECK(fixture.open(QIODevice::ReadOnly));
    auto expected = QJsonDocument::fromJson(fixture.readAll()).object();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "pool-fixture");
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        db.setDatabaseName(temp.path() + "/pool/catalog.db");
        CHECK(db.open());
        QSqlQuery q(db);
        CHECK(q.exec("SELECT version FROM meta"));
        CHECK(q.next());
        CHECK(q.value(0).toInt() == expected["version"].toInt());
        const auto tables = expected["tables"].toObject();
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            CHECK(q.exec("PRAGMA table_info(" + it.key() + ')'));
            QJsonArray actual;
            while (q.next())
                actual.append(q.value(1).toString());
            CHECK(actual == it.value().toArray());
        }
        const auto classes = expected["storageClasses"].toObject();
        CHECK(classes["permanent"].toInt() == int(StorageClass::Permanent));
        CHECK(classes["incoming"].toInt() == int(StorageClass::Incoming));
        CHECK(classes["temporary"].toInt() == int(StorageClass::Temporary));
        CHECK(classes["cache"].toInt() == int(StorageClass::Cache));
        db.close();
    }
    QSqlDatabase::removeDatabase("pool-fixture");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        auto args = app.arguments();
        if (args.size() == 5 && args[1] == "--crash") {
            StoragePool pool(args[2] + "/pool", limits());
            get(pool.open());
            auto ns = QUuid(args[3]), owner = QUuid(args[4]);
            auto id = get(pool.registerNamespace(ns, args[2] + "/root"));
            get(pool.reserve({{}, id, ns, owner, QUuid::createUuid(), StorageClass::Incoming, 700, 0}));
            ::_exit(17);
        }
        admissionAndRecovery();
        rootReplacement();
        concurrentAdmissions();
        crashRecovery();
        schemaFixture();
        std::cout << "storage pool tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
