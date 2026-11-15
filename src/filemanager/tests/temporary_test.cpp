#include <dsfilemanager/storagepool.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QThread>
#include <QProcess>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
#include <source_location>
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
                                 " error=" + std::to_string(int(r.error().code)) +
                                 " native=" + std::to_string(r.error().native_code));
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
    if (r.error().code != expected)
        throw std::runtime_error("expected=" + std::to_string(int(expected)) +
                                 " got=" + std::to_string(int(r.error().code)));
}
void artifacts(PhysicalNaming naming) {
    QTemporaryDir temp;
    auto pool = std::make_shared<StoragePool>(temp.path() + "/pool");
    get(pool->open());
    NativeFileManager manager;
    NamespaceConfig config{QUuid::createUuid(), temp.path() + "/namespace", naming};
    config.storage_pool = pool;
    config.limits.max_temporary_bytes = 8;
    auto ns = get(manager.createNamespace(config));
    CHECK(ns.capabilities.testFlag(Capability::TemporaryArtifacts));
    auto permitted = std::make_shared<std::atomic_bool>(true);
    TemporaryAccess access{QUuid::createUuid(), [permitted] { return permitted->load(); }};
    auto artifact = get(manager.beginTemporary(ns.config.id, {8, StorageClass::Temporary, 300000}, access));
    CHECK(get(manager.list(ns.config.id, ns.root)).entries.isEmpty());
    CHECK(get(manager.listPending(ns.config.id)).entries.isEmpty());
    error(manager.stat(ns.config.id, artifact.info.id), ErrorCode::MissingEntry);
    CHECK(get(artifact.writer->writeAt(0, "test")) == 4);
    get(artifact.writer->checkpoint());
    CHECK(get(artifact.writer->hashPrefix(2))==QCryptographicHash::hash("te",QCryptographicHash::Sha256));
    error(artifact.writer->hashPrefix(5),ErrorCode::IncompleteContent);
    get(artifact.writer->discardSuffix(2));
    CHECK(get(artifact.writer->hashPrefix(2))==QCryptographicHash::hash("te",QCryptographicHash::Sha256));
    CHECK(get(artifact.writer->writeAt(2,"st"))==2);
    error(artifact.writer->commit(), ErrorCode::UnsupportedCapability);
    auto metadata = get(manager.finalizeTemporary(ns.config.id, artifact.info.id, access));
    CHECK(metadata.size == 4 && metadata.sha256 == QCryptographicHash::hash("test", QCryptographicHash::Sha256));
    CHECK(get(manager.finalizeTemporary(ns.config.id, artifact.info.id, access)).revision == metadata.revision);
    error(artifact.writer->writeAt(0, "no"), ErrorCode::Busy);
    auto wrong = access;
    wrong.owner_scope = QUuid::createUuid();
    error(manager.leaseTemporary(ns.config.id, metadata.id, {}, wrong), ErrorCode::MissingEntry);
    auto read = get(manager.leaseTemporary(ns.config.id, metadata.id, {}, access));
    CHECK(get(read.reader->readAt(0, 100)) == "test");
    auto lease = read.lease;
    get(read.reader->close());
    auto resumed = get(manager.leaseTemporary(ns.config.id, metadata.id, {300000, lease}, access));
    CHECK(get(resumed.reader->readAt(1, 2)) == "es");
    auto guard = resumed.reader->accessGuard();
    CHECK(guard());
    get(manager.releaseTemporary(ns.config.id, lease, access));
    CHECK(!guard());
    error(resumed.reader->readAt(0, 4), ErrorCode::MissingEntry);
    get(resumed.reader->close());
    auto other = get(manager.beginTemporary(ns.config.id, {8, StorageClass::Temporary, 300000}, access));
    error(other.writer->writeAt(0, "eight!!!"), ErrorCode::QuotaExceeded);
    auto pending = get(manager.createFile(ns.config.id, {ns.root, "ordinary", ConflictPolicy::Fail, {{1}, {}}}));
    get(pending.writer->writeAt(0, "x"));
    get(pending.writer->commit());
    auto ordinary = get(manager.openRead(ns.config.id, pending.info.file_id));
    CHECK(get(ordinary->readAt(0, 4)) == "x");
    get(ordinary->close());
    get(other.writer->abort());
    get(manager.discardTemporary(ns.config.id, other.info.id, access));
    auto scoped = get(manager.leaseTemporary(ns.config.id, metadata.id, {}, access));
    *permitted = false;
    CHECK(!scoped.reader->accessGuard()());
    error(scoped.reader->readAt(0, 1), ErrorCode::MissingEntry);
    get(scoped.reader->close());
    get(manager.discardTemporary(ns.config.id, metadata.id, access));
    get(manager.discardTemporary(ns.config.id, metadata.id, access));
    *permitted = true;
    auto expiring = get(manager.beginTemporary(ns.config.id, {0, StorageClass::Temporary, 1}, access));
    QThread::msleep(3);
    get(manager.reclaimTemporary(ns.config.id));
    error(expiring.writer->writeAt(0, {}), ErrorCode::MissingEntry);
    get(manager.shutdown());
    get(pool->shutdown());
}
void resumeAfterRestart() {
    QTemporaryDir temp;
    auto pool = std::make_shared<StoragePool>(temp.path() + "/pool");
    get(pool->open());
    NamespaceConfig config{QUuid::createUuid(), temp.path() + "/namespace"};
    config.storage_pool = pool;
    TemporaryAccess access{QUuid::createUuid(), [] { return true; }};
    TempId id;
    QByteArray lease;
    {
        NativeFileManager manager;
        auto ns = get(manager.createNamespace(config));
        auto t = get(manager.beginTemporary(ns.config.id, {8}, access));
        get(t.writer->writeAt(0, "resume"));
        get(t.writer->suspend());
        auto final = get(manager.finalizeTemporary(ns.config.id, t.info.id, access));
        id = final.id;
        auto r = get(manager.leaseTemporary(ns.config.id, id, {}, access));
        lease = r.lease;
        get(r.reader->close());
        get(manager.shutdown());
    }
    {
        NativeFileManager manager;
        get(manager.openNamespace(config));
        auto r = get(manager.leaseTemporary(config.id, id, {300000, lease}, access));
        CHECK(get(r.reader->readAt(0, 20)) == "resume");
        get(r.reader->close());
        get(manager.discardTemporary(config.id, id, access));
        get(manager.shutdown());
    }
    get(pool->shutdown());
}
void incrementalPrefixInvalidation() {
    QTemporaryDir temp;NativeFileManager manager;
    NamespaceConfig config{QUuid::createUuid(),temp.path()+"/namespace"};auto ns=get(manager.createNamespace(config));
    const QByteArray first(65536,'a'),second(65536,'a');
    auto pending=get(manager.createFile(config.id,{ns.root,"prefix",ConflictPolicy::Fail,{{131072},{}}}));
    get(pending.writer->writeAt(0,first));
    CHECK(get(pending.writer->hashPrefix(65536))==QCryptographicHash::hash(first,QCryptographicHash::Sha256));
    qputenv("DSFM_FAIL_AT","before_prefix_read");
    // A repeated durable prefix is obtained without rereading old payload.
    CHECK(get(pending.writer->hashPrefix(65536))==QCryptographicHash::hash(first,QCryptographicHash::Sha256));
    qunsetenv("DSFM_FAIL_AT");
    get(pending.writer->writeAt(65536,second));
    CHECK(get(pending.writer->hashPrefix(131072))==QCryptographicHash::hash(first+second,QCryptographicHash::Sha256));
    get(pending.writer->writeAt(0,"b"));
    qputenv("DSFM_FAIL_AT","before_prefix_read");
    error(pending.writer->hashPrefix(131072),ErrorCode::DiskFull);
    qunsetenv("DSFM_FAIL_AT");
    QByteArray changed=first+second;changed[0]='b';
    CHECK(get(pending.writer->hashPrefix(131072))==QCryptographicHash::hash(changed,QCryptographicHash::Sha256));
    get(pending.writer->discardSuffix(65536));
    CHECK(get(pending.writer->hashPrefix(65536))==QCryptographicHash::hash(changed.first(65536),QCryptographicHash::Sha256));
    get(pending.writer->abort());get(manager.shutdown());
}
void sharedReadOnlySnapshots() {
    QTemporaryDir temp;
    CHECK(QDir().mkdir(temp.path() + "/public"));
    QFile file(temp.path() + "/public/data");
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write("snapshot") == 8);
    file.close();
    auto pool = std::make_shared<StoragePool>(temp.path() + "/pool");
    get(pool->open());
    ReadOnlyNamespaceConfig config;
    config.common = {QUuid::createUuid(), temp.path() + "/public"};
    config.common.storage_pool = pool;
    config.verification = ReadVerification::Snapshot;
    const auto indexed = temp.path() + "/index";
    FileId id;
    QByteArray lease, sha;
    auto settle = [&](ReadOnlyFileManager &manager) {
        for (int i = 0; i < 1000; ++i) {
            auto info = get(manager.namespaceInfo(config.common.id));
            CHECK(!info.readiness_error);
            if (info.status == NamespaceStatus::Ready)
                return;
            QCoreApplication::processEvents();
        }
        throw std::runtime_error("source not ready");
    };
    {
        ReadOnlyFileManager manager(indexed);
        get(manager.createSource(config));
        settle(manager);
        id = get(manager.lookup(config.common.id, "data")).id;
        auto read = get(manager.prepareRead(config.common.id, id));
        CHECK(get(read.reader->readAt(0, 20)) == "snapshot");
        lease = read.resume_lease;
        sha = read.info.sha256;
        auto claims = get(pool->reservations(config.common.id));
        CHECK(claims.size() == 2);
        for (const auto &claim : claims)
            if (claim.storage_class == StorageClass::Cache)
                CHECK(claim.materialized_physical > 0);
        get(read.reader->close());
        get(manager.shutdown());
    }
    {
        ReadOnlyFileManager manager(indexed);
        get(manager.openSource(config));
        settle(manager);
        auto read = get(manager.prepareRead(config.common.id, id, {{}, sha, {}, lease}));
        CHECK(get(read.reader->readAt(0, 20)) == "snapshot");
        get(read.reader->close());
        // Cache exhaustion leaves indexed metadata and other classes usable.
        const auto claims = get(pool->reservations(config.common.id));
        CHECK(claims.size() == 2);
        StoragePoolLimits limits;
        limits.classes[size_t(StorageClass::Cache)].bytes = 0;
        get(pool->setLimits(claims.front().pool, limits));
        error(manager.prepareRead(config.common.id, id), ErrorCode::QuotaExceeded);
        CHECK(get(manager.lookup(config.common.id, "data")).size == 8);
        CHECK(get(pool->reservations(config.common.id)).size() == 2);
        get(manager.shutdown());
    }
    get(pool->shutdown());
}
void settleSource(ReadOnlyFileManager &manager,NamespaceId ns) {
    for(int i=0;i<1000;++i) {
        auto status=get(manager.namespaceInfo(ns));CHECK(!status.readiness_error);
        if(status.status==NamespaceStatus::Ready)return;
        QCoreApplication::processEvents();
    }
    throw std::runtime_error("source scan did not finish");
}
int independentProfileChild(const QStringList &args) {
    auto pool=std::make_shared<StoragePool>(args[2]+"/pool");get(pool->open());
    ReadOnlyFileManager manager(args[2]+"/index");
    ReadOnlyNamespaceConfig config;config.common={QUuid(args[4]),args[3]};config.common.storage_pool=pool;
    config.verification=ReadVerification::Snapshot;
    get(manager.createSource(config));settleSource(manager,config.common.id);
    auto entry=get(manager.lookup(config.common.id,"shared"));auto read=get(manager.prepareRead(config.common.id,entry.id));
    auto token=read.resume_lease;auto digest=read.info.sha256;get(read.reader->close());
    std::cout<<"READY\n"<<std::flush;
    pollfd input{STDIN_FILENO,POLLIN,0};CHECK(::poll(&input,1,10000)==1);
    char command=0;CHECK(::read(STDIN_FILENO,&command,1)==1 && command=='x');
    auto resumed=get(manager.prepareRead(config.common.id,entry.id,{{},digest,{},token}));
    CHECK(get(resumed.reader->readAt(0,32))=="independent");get(resumed.reader->close());
    CHECK(get(pool->reservations(config.common.id)).size()==2);
    get(manager.shutdown());get(pool->shutdown());return 0;
}
void concurrentIndependentProfiles() {
    QTemporaryDir temp;
    for(auto name:{"public","profile-a","profile-b"})CHECK(QDir().mkdir(temp.path()+"/"+name));
    QFile file(temp.path()+"/public/shared");CHECK(file.open(QIODevice::WriteOnly));CHECK(file.write("independent")==11);file.close();
    const auto shared_namespace_id=QUuid::createUuid();
    QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--independent-profile",temp.path()+"/profile-b",
        temp.path()+"/public",shared_namespace_id.toString()});
    CHECK(child.waitForStarted(5000));CHECK(child.waitForReadyRead(5000));CHECK(child.readLine()=="READY\n");
    auto pool=std::make_shared<StoragePool>(temp.path()+"/profile-a/pool");get(pool->open());
    ReadOnlyFileManager manager(temp.path()+"/profile-a/index");
    ReadOnlyNamespaceConfig config;config.common={shared_namespace_id,temp.path()+"/public"};config.common.storage_pool=pool;
    config.verification=ReadVerification::Snapshot;
    get(manager.createSource(config));settleSource(manager,config.common.id);
    auto entry=get(manager.lookup(config.common.id,"shared"));auto read=get(manager.prepareRead(config.common.id,entry.id));
    CHECK(get(read.reader->readAt(0,32))=="independent");get(read.reader->close());
    auto claims=get(pool->reservations(config.common.id));CHECK(claims.size()==2);
    StoragePoolLimits limits;limits.classes[size_t(StorageClass::Cache)].bytes=0;get(pool->setLimits(claims.first().pool,limits));
    error(manager.prepareRead(config.common.id,entry.id),ErrorCode::QuotaExceeded);
    CHECK(child.write("x",1)==1);CHECK(child.waitForBytesWritten(5000));CHECK(child.waitForFinished(5000));
    if(child.exitCode()!=0)throw std::runtime_error("independent profile failed: "+child.readAllStandardError().toStdString());
    get(manager.shutdown());get(pool->shutdown());
}
int crashChild(const QStringList &args) {
    auto pool = std::make_shared<StoragePool>(args[2] + "/pool");
    get(pool->open());
    NativeFileManager manager;
    NamespaceConfig config{QUuid(args[3]), args[2] + "/namespace"};
    config.storage_pool = pool;
    get(manager.openNamespace(config));
    TemporaryAccess access{QUuid(args[4]), [] { return true; }};
    qputenv("DSFM_CRASH_AT", args[6].toLatin1());
    if (args[5] == "birth")
        get(manager.beginTemporary(config.id, {32}, access));
    else if (args[5] == "discard")
        get(manager.discardTemporary(config.id, QUuid(args[7]), access));
    else
        get(manager.finalizeTemporary(config.id, QUuid(args[7]), access));
    return 42;
}
void crashRecovery() {
    for (auto mode : {QString("birth"), QString("finalize"), QString("discard")}) {
        const QStringList points = mode == "birth"
                                       ? QStringList{"after_artifact_intent", "after_artifact_reservation"}
                                       : QStringList{"after_prepare", "after_filesystem", "after_filesystem_record",
                                                     "after_catalog", "after_cleanup",    "after_complete_record"};
        for (const auto &point : points) {
            QTemporaryDir temp;
            NamespaceConfig config{QUuid::createUuid(), temp.path() + "/namespace"};
            TemporaryAccess access{QUuid::createUuid(), [] { return true; }};
            TempId id;
            {
                auto pool = std::make_shared<StoragePool>(temp.path() + "/pool");
                get(pool->open());
                config.storage_pool = pool;
                NativeFileManager manager;
                get(manager.createNamespace(config));
                if (mode != "birth") {
                    auto t = get(manager.beginTemporary(config.id, {32}, access));
                    id = t.info.id;
                    get(t.writer->writeAt(0, "recover"));
                    get(t.writer->suspend());
                    if (mode == "discard")
                        get(manager.finalizeTemporary(config.id, id, access));
                }
                get(manager.shutdown());
                get(pool->shutdown());
                config.storage_pool.reset();
            }
            QProcess process;
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start(QCoreApplication::applicationFilePath(),
                          {"--crash", temp.path(), config.id.toString(), access.owner_scope.toString(), mode, point,
                           id.toString()});
            CHECK(process.waitForStarted(5000));
            CHECK(process.waitForFinished(20000));
            if (process.exitCode() != 86)
                throw std::runtime_error(point.toStdString() + " child=" + std::to_string(process.exitCode()) + " " +
                                         process.readAll().toStdString());
            auto pool = std::make_shared<StoragePool>(temp.path() + "/pool");
            get(pool->open());
            config.storage_pool = pool;
            NativeFileManager manager;
            auto ns = get(manager.openNamespace(config));
            if (mode == "finalize") {
                auto r = get(manager.leaseTemporary(config.id, id, {}, access));
                CHECK(get(r.reader->readAt(0, 32)) == "recover");
                get(r.reader->close());
                struct stat st{};
                CHECK(::stat((config.storage_root + "/.filemanager/artifacts/" + id.toString(QUuid::WithoutBraces))
                                 .toUtf8()
                                 .constData(),
                             &st) == 0);
                CHECK((st.st_mode & 0777) == 0400);
                get(manager.discardTemporary(config.id, id, access));
            } else if (mode == "birth")
                get(manager.reclaimTemporary(config.id));
            auto claims = get(pool->reservations(config.id));
            if (claims.size() != 1)
                throw std::runtime_error(mode.toStdString() + " " + point.toStdString() + " remaining claims=" +
                                         std::to_string(claims.size())); // Namespace metadata only.
            CHECK(get(manager.list(config.id, ns.root)).entries.isEmpty());
            CHECK(get(manager.listPending(config.id)).entries.isEmpty());
            get(manager.shutdown());
            get(pool->shutdown());
        }
    }
}
void pendingPromiseRecovery() {
    QTemporaryDir temp;
    auto pool = std::make_shared<StoragePool>(temp.path() + "/pool");
    get(pool->open());
    NamespaceConfig config{QUuid::createUuid(), temp.path() + "/namespace"};
    config.storage_pool = pool;
    PendingId id;
    quint64 promised = 0;
    {
        NativeFileManager manager;
        auto ns = get(manager.createNamespace(config));
        auto file = get(manager.createFile(config.id, {ns.root, "pending", ConflictPolicy::Fail, {{1048576}, {}}}));
        id = file.info.id;
        get(file.writer->writeAt(0, "prefix"));
        get(file.writer->suspend());
        for (const auto &claim : get(pool->reservations(config.id)))
            if (claim.id == id)
                promised = claim.reserved_total;
        CHECK(promised >= 1048576);
        get(manager.shutdown());
    }
    {
        NativeFileManager manager;
        get(manager.openNamespace(config));
        bool found = false;
        for (const auto &claim : get(pool->reservations(config.id)))
            if (claim.id == id) {
                CHECK(claim.reserved_total == promised);
                found = true;
            }
        CHECK(found);
        auto claims=get(pool->reservations(config.id));
        StoragePoolLimits limits; limits.classes[size_t(StorageClass::Incoming)].bytes=0;
        get(pool->setLimits(claims.front().pool,limits));
        auto writer=get(manager.openPending(config.id,id));
        error(writer.writer->writeAt(6,"more"),ErrorCode::QuotaExceeded);
        get(writer.writer->abort());
        CHECK(get(pool->reservations(config.id)).size() == 1);
        get(manager.shutdown());
    }
    get(pool->shutdown());
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if(app.arguments().value(1)=="--independent-profile")return independentProfileChild(app.arguments());
        if (app.arguments().value(1) == "--crash")
            return crashChild(app.arguments());
        concurrentIndependentProfiles();
        incrementalPrefixInvalidation();
        sharedReadOnlySnapshots();
        crashRecovery();
        pendingPromiseRecovery();
        artifacts(PhysicalNaming::FileId);
        artifacts(PhysicalNaming::OriginalName);
        resumeAfterRestart();
        std::cout << "temporary artifact tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
