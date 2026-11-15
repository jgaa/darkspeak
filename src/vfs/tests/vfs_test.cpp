#include <dsvfs/vfs.h>
#include <dsfilemanager/storagepool.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QTimer>
#include <QThread>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
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
    auto result = QCoro::waitFor(std::move(task));
    if (!result)
        throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                                 " error=" + std::to_string(int(result.error().code)));
    return std::move(result).value();
}
void get(QCoro::Task<Result<void>> task, std::source_location at = std::source_location::current()) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result)
        throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                                 " error=" + std::to_string(int(result.error().code)));
}
template <class T> void error(QCoro::Task<Result<T>> task, ErrorCode expected) {
    auto result = QCoro::waitFor(std::move(task));
    CHECK(!result);
    if (result.error().code != expected)
        throw std::runtime_error("expected=" + std::to_string(int(expected)) +
                                 " actual=" + std::to_string(int(result.error().code)));
}
dsvfs::StorageAssignment assignment(dsvfs::Vfs &vfs, QUuid identity, NamespaceId ns, DirectoryId root,
                                    dsvfs::Request request, bool external = false) {
    auto resources = vfs.resources();
    CHECK(resources);
    QUuid resource;
    for (auto r : resources.value())
        if (r.namespace_id == ns)
            resource = r.id;
    if (resource.isNull()) {
        // Writable test storage is always identity-owned.
        auto created = QCoro::waitFor(vfs.addResource({{}, ns, {}}, request));
        if (!created)
            created = QCoro::waitFor(vfs.addResource({{}, ns, identity}, request));
        CHECK(created);
        resource = created.value().id;
    }
    auto assigned = vfs.assignments(identity);
    CHECK(assigned);
    for (auto a : assigned.value())
        if (a.resource == resource && a.directory == root && a.allow_external_files == external)
            return a;
    return get(vfs.assignStorage({{}, identity, resource, root, external}, request));
}
struct Fixture {
    QTemporaryDir temporary;
    std::shared_ptr<NativeFileManager> backend = std::make_shared<NativeFileManager>();
    std::shared_ptr<StoragePool> pool;
    NamespaceInfo source, exports;
    QUuid identity = QUuid::createUuid();
    std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(1);
    dsvfs::Request request;
    std::unique_ptr<dsvfs::Vfs> vfs;
    Fixture(dsvfs::Limits limits = {}) {
        source = get(backend->createNamespace({QUuid::createUuid(), temporary.path() + "/source"}));
        pool = std::make_shared<StoragePool>(temporary.path() + "/pool");
        get(pool->open());
        NamespaceConfig config{QUuid::createUuid(), temporary.path() + "/exports"};
        config.storage_pool = pool;
        exports = get(backend->createNamespace(config));
        request.scope = QUuid::createUuid();
        request.generation = 1;
        request.current_generation = generation;
        request.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return true; };
        request.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> Result<std::shared_ptr<void>> {
            return std::static_pointer_cast<void>(std::make_shared<int>(1));
        };
        reopen(limits);
    }
    void reopen(dsvfs::Limits limits = {}) {
        if (vfs) {
            get(vfs->shutdown());
            vfs.reset();
        }
        vfs = std::make_unique<dsvfs::Vfs>(temporary.path() + "/vfs", limits);
        get(vfs->open());
        CHECK(vfs->registerNamespace(source.config.id, backend, SymlinkPolicy::Never, identity));
        CHECK(vfs->setExportStore(backend, exports.config.id));
        assignment(*vfs, identity, source.config.id, source.root, request);
    }
    FileInfo file(QString name, QByteArray bytes, QUuid parent = {}) {
        auto pending = get(backend->createFile(
            source.config.id,
            {parent.isNull() ? source.root : parent, name, ConflictPolicy::Fail, {{quint64(bytes.size())}, {}}}));
        CHECK(get(pending.writer->writeAt(0, bytes)) == bytes.size());
        get(pending.writer->markVerified({0, quint64(bytes.size())}));
        get(pending.writer->commit());
        return get(backend->stat(source.config.id, pending.info.file_id));
    }
    dsvfs::Mount mount(QString name = "share", QUuid source_dir = {}, dsvfs::NodeId parent = {}) {
        return get(vfs->addMount(
            identity, {{}, parent, source.config.id, source_dir.isNull() ? source.root : source_dir, name, {}},
            request));
    }
    dsvfs::NodeInfo mounted(QString name = "share") {
        return get(vfs->lookup(identity, vfs->root(identity), name, request));
    }
    ~Fixture() {
        if (vfs) {
            auto ignored = QCoro::waitFor(vfs->shutdown());
        }
        QCoro::waitFor(backend->shutdown());
    }
};
void identitiesAndPersistence() {
    Fixture f;
    auto original = f.file("hello", "hello");
    auto mount = f.mount();
    auto mount_node = f.mounted();
    auto child = get(f.vfs->lookup(f.identity, mount_node.id, "hello", f.request));
    CHECK(child.id.size() == 105);
    CHECK(!child.id.contains(original.id.toRfc4122()));
    auto reader = get(f.vfs->prepareRead(f.identity, child.id, {}, f.request));
    CHECK(get(reader.reader->readAt(0, 100)) == "hello");
    f.reopen();
    CHECK(f.mounted().id == mount_node.id);
    CHECK(get(f.vfs->stat(f.identity, child.id, f.request)).name == "hello");
    get(f.vfs->changeMount(f.identity, mount.id, f.vfs->root(f.identity), "renamed", f.request));
    CHECK(f.mounted("renamed").id == mount_node.id);
    CHECK(get(f.vfs->stat(f.identity, child.id, f.request)).id == child.id);
    auto tampered = child.id;
    tampered[35] = char(tampered[35] ^ 1);
    error(f.vfs->stat(f.identity, tampered, f.request), ErrorCode::MissingEntry);
    error(f.vfs->stat(QUuid::createUuid(), child.id, f.request), ErrorCode::MissingEntry);
    get(f.vfs->removeMount(f.identity, mount.id, f.request));
    error(f.vfs->stat(f.identity, child.id, f.request), ErrorCode::MissingEntry);
    f.mount("renamed");
    CHECK(f.mounted("renamed").id != mount_node.id);
}
void authorizationAndPaging() {
    Fixture f;
    auto hidden = f.file("hidden", "private");
    f.file("a", "a");
    f.file("b", "b");
    auto shared = f.mount();
    auto private_mount = f.mount("private");
    auto node = f.mounted();
    auto request = f.request;
    auto denied_ids = std::make_shared<QSet<QUuid>>();
    denied_ids->insert(hidden.id);
    request.authorize = [denied_ids, private_mount](dsvfs::Target target, dsvfs::Operation) -> Result<bool> {
        return target.mount != private_mount.id && !denied_ids->contains(target.source);
    };
    auto root = get(f.vfs->list(f.identity, f.vfs->root(f.identity), {100, {}}, request));
    CHECK(root.entries.size() == 1);
    auto page = get(f.vfs->list(f.identity, node.id, {1, {}}, request));
    CHECK(page.entries.size() == 1 && page.entries[0].name == "a" && !page.next_cursor.isEmpty());
    auto before = get(f.vfs->stat(f.identity, node.id, request));
    auto denied = request;
    denied.scope = QUuid::createUuid();
    error(f.vfs->list(f.identity, node.id, {1, page.next_cursor}, denied), ErrorCode::StaleCursor);
    denied_ids->insert(f.file("hidden2", "also private").id); // Frozen cursors survive unrelated source updates.
    auto second = get(f.vfs->list(f.identity, node.id, {1, page.next_cursor}, request));
    CHECK(second.entries.size() == 1 && second.entries[0].name == "b");
    CHECK(get(f.vfs->stat(f.identity, node.id, request)).revision == before.revision);
    auto wrong_subtree_id = get(f.vfs->stat(f.identity, node.id, request));
    (void)wrong_subtree_id;
    auto backend_id = hidden.id.toRfc4122();
    error(f.vfs->stat(f.identity, backend_id, request), ErrorCode::MissingEntry);
    auto file = get(f.vfs->lookup(f.identity, node.id, "a", request));
    auto read = get(f.vfs->prepareRead(f.identity, file.id, {}, request));
    ++*f.generation;
    error(read.reader->readAt(0, 1), ErrorCode::MissingEntry);
    error(f.vfs->list(f.identity, node.id, {1, page.next_cursor}, request), ErrorCode::StaleCursor);
}
void subtreeAndMutations() {
    Fixture f;
    auto sub = get(f.backend->mkdir(f.source.config.id, f.source.root, "sub"));
    f.file("inside", "ok", sub);
    f.file("outside", "no");
    auto all = f.mount("all");
    auto sub_mount = f.mount("sub", sub);
    auto all_node = f.mounted("all"), sub_node = f.mounted("sub");
    auto inside = get(f.vfs->lookup(f.identity, sub_node.id, "inside", f.request));
    auto pending =
        get(f.vfs->createFile(f.identity, sub_node.id, {{}, "new", ConflictPolicy::Fail, {{3}, {}}}, f.request));
    CHECK(get(pending.writer->writeAt(0, "new")) == 3);
    get(pending.writer->markVerified({0, 3}));
    get(pending.writer->commit());
    auto created = get(f.vfs->lookup(f.identity, sub_node.id, "new", f.request));
    error(f.vfs->move(f.identity, created.id, all_node.id, f.request), ErrorCode::CrossNamespaceMove);
    auto nested = f.mount("nested", f.source.root, sub_node.id);
    error(f.vfs->remove(f.identity, sub_node.id, f.request), ErrorCode::UnsupportedCapability);
    error(f.vfs->removeMount(f.identity, sub_mount.id, f.request),
          ErrorCode::DirectoryNotEmpty); // sub mount is synthetic-root sibling, nested belongs to sub mount, not all
    get(f.vfs->removeMount(f.identity, nested.id, f.request));
    auto source_inside = get(f.backend->list(f.source.config.id, sub));
    FileId inside_id;
    for (auto item : source_inside.entries)
        if (item.name == "inside")
            inside_id = item.id;
    get(f.backend->move(f.source.config.id, inside_id, f.source.root));
    error(f.vfs->stat(f.identity, inside.id, f.request), ErrorCode::MissingEntry);
    (void)all;
}
void conflictsAndRevocation() {
    Fixture f;
    auto directory = get(f.backend->mkdir(f.source.config.id, f.source.root, "dir"));
    auto outer = f.mount();
    auto outer_node = f.mounted();
    auto parent = get(f.vfs->lookup(f.identity, outer_node.id, "dir", f.request));
    auto nested = f.mount("nested", f.source.root, parent.id);
    int notifications = 0, resolved = 0;
    QObject::connect(f.vfs.get(), &dsvfs::Vfs::conflictDetected, [&](QUuid, QByteArray, QString) { ++notifications; });
    QObject::connect(f.vfs.get(), &dsvfs::Vfs::conflictResolved, [&](QUuid, QByteArray) { ++resolved; });
    auto collision = f.file("nested", "collision", directory);
    get(f.vfs->refresh());
    CHECK(notifications == 1);
    get(f.vfs->refresh());
    CHECK(notifications == 1);
    error(f.vfs->lookup(f.identity, parent.id, "nested", f.request), ErrorCode::MissingEntry);
    get(f.backend->remove(f.source.config.id, collision.id));
    get(f.vfs->refresh());
    CHECK(resolved == 1);
    error(f.vfs->remove(f.identity, parent.id, f.request), ErrorCode::DirectoryNotEmpty);
    error(f.vfs->changeMount(f.identity, outer.id, parent.id, "cycle", f.request), ErrorCode::Conflict);
    auto file = f.file("data", "payload");
    auto data = get(f.vfs->lookup(f.identity, outer_node.id, "data", f.request));
    auto read = get(f.vfs->prepareRead(f.identity, data.id, {}, f.request));
    get(f.vfs->removeMount(f.identity, nested.id, f.request));
    get(f.vfs->removeMount(f.identity, outer.id, f.request));
    error(read.reader->readAt(0, 7), ErrorCode::MissingEntry);
    (void)file;
}
void exportsAndValidation() {
    Fixture f;
    f.file("a", "hello");
    auto dir = get(f.backend->mkdir(f.source.config.id, f.source.root, "dir"));
    f.file("b", "world", dir);
    f.mount();
    auto catalog = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request));
    CHECK(catalog->size() > 0 && catalog->sha256().size() == 32);
    CHECK(get(f.backend->list(f.exports.config.id, f.exports.root)).entries.isEmpty());
    CHECK(get(f.backend->listPending(f.exports.config.id)).entries.isEmpty());
    auto prepared = get(catalog->prepareRead());
    CHECK(prepared.info.sha256 == catalog->sha256());
    auto reader = std::shared_ptr<IFileReader>(std::move(prepared.reader));
    get(f.vfs->validateCatalog(reader, catalog->size(), catalog->sha256()));
    auto bad_hash = catalog->sha256();
    bad_hash[0] = char(bad_hash[0] ^ 1);
    error(f.vfs->validateCatalog(reader, catalog->size(), bad_hash), ErrorCode::IntegrityError);
    auto resumed = get(catalog->prepareRead({{}, catalog->sha256(), {}, prepared.resume_lease}));
    CHECK(get(resumed.reader->readAt(0, 50)).size() == 50);
    ++*f.generation;
    error(resumed.reader->readAt(0, 10), ErrorCode::MissingEntry);
    error(catalog->prepareRead(), ErrorCode::MissingEntry);
    get(catalog->close());
    get(f.vfs->refresh());
    CHECK(get(f.backend->list(f.exports.config.id, f.exports.root)).entries.isEmpty());
}
void subscriptionsAndHiddenChanges() {
    Fixture f;
    auto hidden = f.file("hidden", "private");
    f.file("a", "a");
    f.mount();
    auto node = f.mounted();
    auto request = f.request;
    request.authorize = [hidden](dsvfs::Target target, dsvfs::Operation) -> Result<bool> {
        return target.source != hidden.id;
    };
    QObject receiver;
    int changes = 0;
    bool invalidated = false;
    auto subscription = get(f.vfs->subscribeDirectory(f.identity, node.id,
                                                      {&receiver,
                                                       [&](dsvfs::Change change) {
                                                           ++changes;
                                                           invalidated = change.invalidated;
                                                       }},
                                                      request));
    auto pending = get(f.backend->replaceFile(f.source.config.id, hidden.id, {{7}, {}}));
    get(pending.writer->writeAt(0, "changed"));
    get(pending.writer->markVerified({0, 7}));
    get(pending.writer->commit());
    get(f.vfs->refresh());
    QCoreApplication::processEvents();
    CHECK(changes == 0);
    f.file("b", "b");
    get(f.vfs->refresh());
    QCoreApplication::processEvents();
    CHECK(changes == 1 && !invalidated);
    ++*f.generation;
    get(f.vfs->refresh());
    QCoreApplication::processEvents();
    CHECK(changes == 2 && invalidated);
}
void readOnlyAliases() {
    QTemporaryDir temporary;
    CHECK(QDir().mkdir(temporary.path() + "/a"));
    CHECK(QDir().mkdir(temporary.path() + "/b"));
    CHECK(QDir().mkdir(temporary.path() + "/b/dir"));
    auto write = [](QString path, QByteArray bytes) {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write(bytes) == bytes.size());
    };
    write(temporary.path() + "/b/dir/hello", "hello");
    write(temporary.path() + "/b/dir/secret", "secret");
    CHECK(::symlink(QFile::encodeName(temporary.path() + "/b/dir").constData(),
                    QFile::encodeName(temporary.path() + "/a/link").constData()) == 0);
    auto backend = std::make_shared<ReadOnlyFileManager>(temporary.path() + "/index");
    ReadOnlyNamespaceConfig a;
    a.common = {QUuid::createUuid(), temporary.path() + "/a"};
    a.symlinks = SymlinkPolicy::WithinVfs;
    a.exclusions.objects.append(temporary.path() + "/b/dir/secret");
    ReadOnlyNamespaceConfig b;
    b.common = {QUuid::createUuid(), temporary.path() + "/b"};
    b.verification = ReadVerification::Snapshot;
    auto source_a = get(backend->createSource(a));
    auto source_b = get(backend->createSource(b));
    for (auto ns : {a.common.id, b.common.id}) {
        bool ready = false;
        for (int turn = 0; turn < 1000; ++turn)
            if (get(backend->namespaceInfo(ns)).status == NamespaceStatus::Ready) {
                ready = true;
                break;
            }
        CHECK(ready);
    }
    auto dir = get(backend->lookup(b.common.id, "dir"));
    auto hello = get(backend->lookup(b.common.id, "dir/hello"));
    auto secret = get(backend->lookup(b.common.id, "dir/secret"));
    dsvfs::Vfs vfs(temporary.path() + "/vfs");
    get(vfs.open());
    CHECK(vfs.registerNamespace(a.common.id, backend, SymlinkPolicy::WithinVfs));
    CHECK(vfs.registerNamespace(b.common.id, backend));
    auto identity = QUuid::createUuid();
    dsvfs::Request request;
    request.scope = QUuid::createUuid();
    request.current_generation = std::make_shared<std::atomic<quint64>>(0);
    request.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return true; };
    request.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> Result<std::shared_ptr<void>> {
        return std::static_pointer_cast<void>(std::make_shared<int>(0));
    };
    assignment(vfs, identity, a.common.id, source_a.root, request);
    assignment(vfs, identity, b.common.id, dir.id, request);
    auto mount_a = get(vfs.addMount(identity, {{}, {}, a.common.id, source_a.root, "a", {}}, request));
    auto mount_b = get(vfs.addMount(identity, {{}, {}, b.common.id, dir.id, "b", {}}, request));
    struct stat physical{};
    CHECK(::stat(QFile::encodeName(temporary.path() + "/b/dir").constData(), &physical) == 0);
    request.resolve_target = [=](FilesystemIdentity object) -> Result<dsvfs::Target> {
        if (object.device != quint64(physical.st_dev) || object.inode != quint64(physical.st_ino))
            return Error{ErrorCode::MissingEntry};
        return dsvfs::Target{identity, mount_b.id, b.common.id, dir.id};
    };
    request.within_subtree = [=](dsvfs::Target target, QUuid root) {
        return target.mount == mount_b.id && root == dir.id &&
               (target.source == dir.id || target.source == hello.id || target.source == secret.id);
    };
    auto a_node = get(vfs.lookup(identity, vfs.root(identity), "a", request));
    auto link = get(vfs.lookup(identity, a_node.id, "link", request));
    auto children = get(vfs.list(identity, link.id, {100, {}}, request));
    CHECK(children.entries.size() == 1 && children.entries[0].name == "hello");
    auto routed = children.entries[0];
    CHECK(routed.id[0] == 2);
    CHECK(routed.parent == link.id);
    CHECK(get(vfs.stat(identity, routed.id, request)).parent == link.id);
    auto read = get(vfs.prepareRead(identity, routed.id, {}, request));
    CHECK(get(read.reader->readAt(0, 5)) == "hello");
    get(read.reader->close());
    CHECK(!read.resume_lease.isEmpty());
    QFile lease_fixture(QStringLiteral(VFS_FIXTURES) + "/lease-v1.json");
    CHECK(lease_fixture.open(QIODevice::ReadOnly));
    const auto lease_format = QJsonDocument::fromJson(lease_fixture.readAll()).object();
    CHECK(quint8(read.resume_lease[0]) == lease_format["prefix"].toInt());
    auto other_scope = request;
    other_scope.scope = QUuid::createUuid();
    error(vfs.prepareRead(identity, routed.id, {{read.info.revision}, read.info.sha256, {}, read.resume_lease},
                          other_scope),
          ErrorCode::MissingEntry);
    write(temporary.path() + "/b/dir/hello", "changed live file");
    get(backend->reconcile(b.common.id, {}));
    for (int turn = 0; turn < 1000; ++turn)
        if (get(backend->namespaceInfo(b.common.id)).status == NamespaceStatus::Ready)
            break;
    auto resumed = get(
        vfs.prepareRead(identity, routed.id, {{read.info.revision}, read.info.sha256, {}, read.resume_lease}, request));
    CHECK(get(resumed.reader->readAt(0, 100)) == "hello");
    get(resumed.reader->close());
    auto direct_mount = get(vfs.lookup(identity, vfs.root(identity), "b", request));
    auto direct_file = get(vfs.lookup(identity, direct_mount.id, "hello", request));
    error(vfs.prepareRead(identity, direct_file.id, {{read.info.revision}, read.info.sha256, {}, read.resume_lease},
                          request),
          ErrorCode::MissingEntry);
    auto denied = request;
    denied.authorize = [=](dsvfs::Target target, dsvfs::Operation) -> Result<bool> {
        return target.mount != mount_b.id;
    };
    error(vfs.stat(identity, routed.id, denied), ErrorCode::MissingEntry);
    error(vfs.lookup(identity, a_node.id, "link", denied), ErrorCode::MissingEntry);
    auto second_identity = QUuid::createUuid();
    assignment(vfs, second_identity, b.common.id, dir.id, request);
    auto shared = get(vfs.addMount(second_identity, {{}, {}, b.common.id, dir.id, "shared", {}}, request));
    auto shared_node = get(vfs.lookup(second_identity, vfs.root(second_identity), "shared", request));
    auto shared_hello = get(vfs.lookup(second_identity, shared_node.id, "hello", request));
    CHECK(shared_hello.id != routed.id);
    CHECK(get(backend->namespaces()).size() == 2);
    auto export_backend = std::make_shared<NativeFileManager>();
    auto pool = std::make_shared<StoragePool>(temporary.path() + "/pool");
    get(pool->open());
    NamespaceConfig export_config{QUuid::createUuid(), temporary.path() + "/exports"};
    export_config.storage_pool = pool;
    auto export_space = get(export_backend->createNamespace(export_config));
    CHECK(vfs.setExportStore(export_backend, export_space.config.id));
    auto exported = get(vfs.exportCatalog(identity, link.id, false, request));
    auto opened_before_exclusion = get(vfs.prepareRead(identity, routed.id, {}, request));
    a.exclusions.generation = 2;
    a.exclusions.objects.append(temporary.path() + "/b/dir/hello");
    get(backend->updateExclusions(a.common.id, a.exclusions));
    error(opened_before_exclusion.reader->readAt(0, 5), ErrorCode::MissingEntry);
    CHECK(!exported->isAuthorized());
    error(exported->prepareRead(), ErrorCode::MissingEntry);
    get(exported->close());
    get(vfs.refresh());
    auto direct_b = get(vfs.lookup(identity, vfs.root(identity), "b", request));
    CHECK(get(vfs.list(identity, direct_b.id, {100, {}}, request)).entries.size() == 2);
    auto filtered = request;
    filtered.authorize = [=](dsvfs::Target target, dsvfs::Operation) -> Result<bool> {
        return target.namespace_id != b.common.id || target.source == dir.id || target.source == hello.id;
    };
    auto before_hidden = get(vfs.stat(identity, direct_b.id, filtered));
    QObject receiver;
    int hidden_notifications = 0;
    auto subscription = get(vfs.subscribeDirectory(
        identity, direct_b.id, {&receiver, [&](dsvfs::Change) { ++hidden_notifications; }}, filtered));
    write(temporary.path() + "/b/dir/new-hidden", "private");
    // stat detects the physical directory fingerprint change before reconciliation.
    auto after_hidden = get(vfs.stat(identity, direct_b.id, filtered));
    CHECK(after_hidden.status == before_hidden.status && after_hidden.revision == before_hidden.revision);
    get(backend->reconcile(b.common.id, {}));
    for (int turn = 0; turn < 1000; ++turn)
        if (get(backend->namespaceInfo(b.common.id)).status == NamespaceStatus::Ready)
            break;
    get(vfs.refresh());
    QCoreApplication::processEvents();
    CHECK(hidden_notifications == 0);
    CHECK(get(vfs.stat(identity, direct_b.id, filtered)).revision == before_hidden.revision);
    CHECK(get(vfs.list(identity, direct_b.id, {100, {}}, filtered)).entries.size() == 1);
    get(vfs.shutdown());
    get(export_backend->shutdown());
    get(backend->shutdown());
    (void)shared;
    (void)source_b;
    (void)mount_a;
}
class MemoryReader final : public IFileReader {
  public:
    explicit MemoryReader(QByteArray bytes) : bytes_(std::move(bytes)) {}
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype count, CancellationToken cancel) override {
        if (cancel.cancelled())
            co_return Error{ErrorCode::Cancelled};
        if (count < 0)
            co_return Error{ErrorCode::InvalidArgument};
        if (offset >= quint64(bytes_.size()))
            co_return QByteArray{};
        co_return bytes_.sliced(qsizetype(offset), std::min(count, bytes_.size() - qsizetype(offset)));
    }
    QCoro::Task<Result<void>> close() override { co_return Result<void>{}; }

  private:
    QByteArray bytes_;
};
void formatsAndBounds() {
    Fixture f;
    QFile fixture(QStringLiteral(VFS_FIXTURES) + "/catalog-v1.jsonl");
    CHECK(fixture.open(QIODevice::ReadOnly));
    auto bytes = fixture.readAll();
    auto reader = std::make_shared<MemoryReader>(bytes);
    get(f.vfs->validateCatalog(reader, bytes.size(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)));
    auto hostile = bytes;
    hostile.replace("\"version\":1", "\"version\":2");
    error(f.vfs->validateCatalog(std::make_shared<MemoryReader>(hostile), hostile.size(),
                                 QCryptographicHash::hash(hostile, QCryptographicHash::Sha256)),
          ErrorCode::Corruption);
    hostile = bytes;
    hostile.replace("\"name\":\"/\"", "\"name\":\"..\"");
    error(f.vfs->validateCatalog(std::make_shared<MemoryReader>(hostile), hostile.size(),
                                 QCryptographicHash::hash(hostile, QCryptographicHash::Sha256)),
          ErrorCode::Corruption);
    hostile = bytes + bytes.mid(bytes.indexOf('\n') + 1); // Duplicate root, valid whole-payload digest.
    error(f.vfs->validateCatalog(std::make_shared<MemoryReader>(hostile), hostile.size(),
                                 QCryptographicHash::hash(hostile, QCryptographicHash::Sha256)),
          ErrorCode::Corruption);
    auto cancelled = CancellationToken{};
    cancelled.cancel();
    error(f.vfs->validateCatalog(reader, bytes.size(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256),
                                 cancelled),
          ErrorCode::Cancelled);
    f.file("a", "a");
    f.file("b", "b");
    f.mount();
    auto first = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request));
    auto second = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request));
    error(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request), ErrorCode::QueueFull);
    get(first->close());
    get(second->close());
    get(f.vfs->refresh());
    error(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request, cancelled), ErrorCode::Cancelled);
    CHECK(get(f.backend->list(f.exports.config.id, f.exports.root)).entries.isEmpty());
    dsvfs::Limits limits;
    limits.max_export_entries = 1;
    f.reopen(limits);
    error(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request), ErrorCode::QuotaExceeded);
    CHECK(get(f.backend->list(f.exports.config.id, f.exports.root)).entries.isEmpty());
    error(f.vfs->list(f.identity, f.vfs->root(f.identity), {101, {}}, f.request), ErrorCode::InvalidArgument);
    auto missing_context = f.request;
    missing_context.current_generation.reset();
    error(f.vfs->stat(f.identity, f.vfs->root(f.identity), missing_context), ErrorCode::MissingEntry);
}
void nodeRegressionFixture() {
    QTemporaryDir temporary;
    CHECK(QFile::copy(QStringLiteral(VFS_FIXTURES) + "/mounts-v2.json", temporary.filePath("mounts.json")));
    CHECK(QFile::setPermissions(temporary.filePath("mounts.json"), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QFile fixture(QStringLiteral(VFS_FIXTURES) + "/node-v1.json");
    CHECK(fixture.open(QIODevice::ReadOnly));
    auto record = QJsonDocument::fromJson(fixture.readAll()).object();
    dsvfs::Vfs vfs(temporary.path());
    get(vfs.open());
    auto id = vfs.nodeId(QUuid(record["identity"].toString()), QUuid(record["mount"].toString()),
                         QUuid(record["source"].toString()));
    CHECK(id);
    CHECK(id.value().toHex() == record["node"].toString().toLatin1());
    get(vfs.shutdown());
}
void writerRevocationAndOwnership() {
    Fixture f;
    auto mounted = f.mount();
    auto node = f.mounted();
    auto other_identity = QUuid::createUuid();
    error(f.vfs->addMount(other_identity, {{}, {}, f.source.config.id, f.source.root, "forbidden", {}}, f.request),
          ErrorCode::PermissionDenied);
    auto pending =
        get(f.vfs->createFile(f.identity, node.id, {{}, "pending", ConflictPolicy::Fail, {{3}, {}}}, f.request));
    get(pending.writer->writeAt(0, "abc"));
    get(pending.writer->markVerified({0, 3}));
    ++*f.generation;
    error(pending.writer->commit(), ErrorCode::MissingEntry);
    CHECK(!QCoro::waitFor(f.backend->lookup(f.source.config.id, "pending")));
    f.request.generation = f.generation->load();
    auto no_commit = f.request;
    no_commit.commit_permit = {};
    error(f.vfs->mkdir(f.identity, node.id, "no-permit", no_commit), ErrorCode::PermissionDenied);
    auto upload = f.request;
    upload.authorize = [](dsvfs::Target, dsvfs::Operation operation) -> Result<bool> {
        return operation != dsvfs::Operation::Write;
    };
    auto file = get(f.vfs->createFile(f.identity, node.id, {{}, "upload", ConflictPolicy::Fail, {{3}, {}}}, upload));
    get(file.writer->writeAt(0, "new"));
    get(file.writer->markVerified({0, 3}));
    get(file.writer->commit());
    get(f.vfs->removeMount(f.identity, mounted.id, f.request));
}
void asciiPresentationAndStaleAliasProof() {
    Fixture f;
    f.file(QString::fromUtf8("é%"), "content");
    f.mount();
    auto request = f.request;
    request.allow_utf8 = false;
    auto root = f.mounted();
    auto entry = get(f.vfs->lookup(f.identity, root.id, "%C3%A9%25", request));
    CHECK(entry.name == "%C3%A9%25");
    auto catalog = get(f.vfs->exportCatalog(f.identity, root.id, false, request));
    CHECK(!catalog->requiresUtf8());
    auto prepared = get(catalog->prepareRead());
    auto reader = std::shared_ptr<IFileReader>(std::move(prepared.reader));
    get(f.vfs->validateCatalog(reader, catalog->size(), catalog->sha256()));
    get(catalog->close());
    get(f.vfs->refresh());
    auto directory = get(f.backend->mkdir(f.source.config.id, f.source.root, "subtree"));
    auto file = f.file("file", "secret after move", directory);
    QTemporaryDir source;
    CHECK(source.isValid());
    const auto id = file.id.toString(QUuid::WithoutBraces);
    const auto physical = f.source.config.storage_root + "/objects/" + id.left(2) + "/" + id;
    CHECK(::symlink(QFile::encodeName(physical).constData(), QFile::encodeName(source.filePath("alias")).constData()) ==
          0);
    auto readonly = std::make_shared<ReadOnlyFileManager>(f.temporary.filePath("readonly-index"));
    ReadOnlyNamespaceConfig config;
    config.common = {QUuid::createUuid(), source.path()};
    config.symlinks = SymlinkPolicy::WithinVfs;
    auto ns = get(readonly->createSource(config));
    for (int turn = 0; turn < 1000; ++turn)
        if (get(readonly->namespaceInfo(config.common.id)).status == NamespaceStatus::Ready)
            break;
    CHECK(f.vfs->registerNamespace(config.common.id, readonly, SymlinkPolicy::WithinVfs));
    auto destination = f.mount("destination", directory);
    assignment(*f.vfs, f.identity, config.common.id, ns.root, f.request);
    get(f.vfs->addMount(f.identity, {{}, {}, config.common.id, ns.root, "aliases", {}}, f.request));
    struct stat identity{};
    CHECK(::stat(QFile::encodeName(physical).constData(), &identity) == 0);
    auto alias_request = f.request;
    alias_request.resolve_target =
        [=, owner = f.identity, namespace_id = f.source.config.id](FilesystemIdentity object) -> Result<dsvfs::Target> {
        if (object.device != quint64(identity.st_dev) || object.inode != quint64(identity.st_ino))
            return Error{ErrorCode::MissingEntry};
        return dsvfs::Target{owner, destination.id, namespace_id, file.id};
    };
    // Deliberately stale host evidence must never be sufficient to read an
    // object that the destination catalog places outside the mounted subtree.
    alias_request.within_subtree = [](dsvfs::Target, QUuid) { return true; };
    auto aliases = get(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "aliases", alias_request));
    auto alias = get(f.vfs->lookup(f.identity, aliases.id, "alias", alias_request));
    auto before = get(f.vfs->prepareRead(f.identity, alias.id, {}, alias_request));
    CHECK(get(before.reader->readAt(0, 50)) == "secret after move");
    get(before.reader->close());
    get(f.backend->move(f.source.config.id, file.id, f.source.root));
    error(f.vfs->stat(f.identity, alias.id, alias_request), ErrorCode::MissingEntry);
    error(f.vfs->prepareRead(f.identity, alias.id, {}, alias_request), ErrorCode::MissingEntry);
    // VFS owns this backend registration until shutdown.
}
void boundedCollectionPaging() {
    QTemporaryDir temporary;
    CHECK(QDir().mkdir(temporary.filePath("source")));
    for (int i = 0; i < 1200; ++i) {
        QFile file(temporary.filePath(QStringLiteral("source/entry-%1").arg(i, 5, 10, QChar('0'))));
        CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write("x") == 1);
    }
    auto backend = std::make_shared<ReadOnlyFileManager>(temporary.filePath("index"));
    ReadOnlyNamespaceConfig config;
    config.common = {QUuid::createUuid(), temporary.filePath("source")};
    config.common.limits.max_batch = 256;
    auto source = get(backend->createSource(config));
    bool ready = false;
    for (int turn = 0; turn < 5000; ++turn)
        if (get(backend->namespaceInfo(config.common.id)).status == NamespaceStatus::Ready) {
            ready = true;
            break;
        }
    CHECK(ready);
    dsvfs::Vfs vfs(temporary.filePath("vfs"));
    get(vfs.open());
    CHECK(vfs.registerNamespace(config.common.id, backend));
    auto identity = QUuid::createUuid();
    dsvfs::Request request;
    request.scope = QUuid::createUuid();
    request.current_generation = std::make_shared<std::atomic<quint64>>(0);
    request.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return true; };
    request.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> Result<std::shared_ptr<void>> {
        return std::static_pointer_cast<void>(std::make_shared<int>(0));
    };
    assignment(vfs, identity, config.common.id, source.root, request);
    auto mount = get(vfs.addMount(identity, {{}, {}, config.common.id, source.root, "collection", {}}, request));
    auto root = get(vfs.lookup(identity, vfs.root(identity), "collection", request));
    QTimer timer;
    timer.setInterval(1);
    int heartbeats = 0;
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeats; });
    timer.start();
    PageRequest page{37, {}};
    int count = 0;
    QByteArray previous;
    do {
        auto result = get(vfs.list(identity, root.id, page, request));
        CHECK(result.entries.size() <= 37);
        for (const auto &entry : result.entries) {
            CHECK(previous.isEmpty() || previous < entry.name.toUtf8());
            previous = entry.name.toUtf8();
            ++count;
        }
        page.cursor = result.next_cursor;
    } while (!page.cursor.isEmpty());
    CHECK(count == 1200);
    CHECK(heartbeats > 20);
    QFile status("/proc/self/status");
    CHECK(status.open(QIODevice::ReadOnly));
    const auto lines = status.readAll().split('\n');
    for (auto line : lines)
        if (line.startsWith("VmRSS:")) {
            auto fields = line.simplified().split(' ');
            CHECK(fields.size() >= 2 && fields[1].toULongLong() < 131072);
        }
    get(vfs.shutdown());
    get(vfs.shutdown());
    get(backend->shutdown());
    (void)mount;
}
#include "policy_cases.h"
#include "architecture_cases.h"
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        authorizationFixture();
        permissionSemantics();
        permissionProvenance();
        overlaySemantics();
        assignmentBoundaries();
        effectiveTopology();
        navigationOnlyReopening();
        boundedFilteredContinuation();
        externalFilesOnly();
        std::cout << "architecture regressions passed\n";
        if (app.arguments().contains("--architecture-only"))
            return 0;
        boundedCollectionPaging();
        std::cout << "bounded collection paging passed\n";
        asciiPresentationAndStaleAliasProof();
        std::cout << "ASCII/stale alias proof passed\n";
        nodeRegressionFixture();
        std::cout << "node fixture passed\n";
        formatsAndBounds();
        std::cout << "formats/bounds passed\n";
        writerRevocationAndOwnership();
        std::cout << "writers/ownership passed\n";
        identitiesAndPersistence();
        std::cout << "identities/persistence passed\n";
        authorizationAndPaging();
        std::cout << "authorization/paging passed\n";
        subtreeAndMutations();
        std::cout << "subtrees/mutations passed\n";
        conflictsAndRevocation();
        std::cout << "conflicts/revocation passed\n";
        exportsAndValidation();
        std::cout << "exports/validation passed\n";
        readOnlyAliases();
        std::cout << "read-only aliases passed\n";
        subscriptionsAndHiddenChanges();
        std::cout << "subscriptions/privacy passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
