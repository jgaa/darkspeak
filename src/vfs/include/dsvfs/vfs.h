#pragma once

#include <dsvfs/policy.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include <QObject>
#include <QJsonObject>

namespace dsvfs {
using NodeId = QByteArray;
using dsfm::CancellationToken;
using dsfm::Result;
struct EffectiveVfs;
struct Target {
    QUuid identity;
    QUuid mount;
    dsfm::NamespaceId namespace_id;
    QUuid source;
};
// Trusted host state, never deserialized from a remote request. Callbacks must
// be bounded/thread-safe and must not perform reentrant storage operations.
struct Request {
    QUuid scope;
    bool allow_utf8 = true; // False presents logical names as reversible ASCII percent escapes.
    quint64 generation = 0;
    std::shared_ptr<const std::atomic<quint64>> current_generation;
    std::function<Result<bool>(Target, Operation)> authorize;
    // Resolves to one explicitly selected, authorized mount. Unknown targets
    // fail closed. The host supplies current in-memory subtree evidence.
    std::function<Result<Target>(dsfm::FilesystemIdentity)> resolve_target;
    std::function<bool(Target, QUuid)> within_subtree;
    // Holds the host's revocation serialization until publication completes.
    // Required for mutations; permission withdrawal must wait for this permit.
    std::function<Result<std::shared_ptr<void>>(Target, Operation)> commit_permit;
    QSet<QUuid> groups; // trusted identity-scoped host membership
    std::shared_ptr<const PermissionPolicy> permissions;
    std::shared_ptr<const EffectiveVfs> effective; // obtained through compose()
    bool current() const noexcept;
};
struct Limits {
    quint32 max_mounts = 128;
    quint32 max_depth = 64;
    quint32 max_page_size = 100;
    quint32 max_cursors = 128;
    quint32 max_subscriptions = 128;
    quint32 max_requests = 32;
    quint32 max_readers = 128;
    quint32 reader_lifetime_ms = 300000;
    quint32 max_batch = 100;
    quint32 max_export_entries = 1000000;
    quint64 max_export_bytes = 256ULL << 20;
    quint32 max_exports_per_identity = 2;
    quint32 max_exports_per_contact = 2;
    quint32 max_exports = 2; // Generation and retained/transmitting exports.
    quint32 max_record_bytes = 16384;
    quint32 request_timeout_ms = 30000;
    quint32 export_timeout_ms = 300000;
    quint32 cursor_lifetime_ms = 60000;
    quint32 export_lifetime_ms = 300000;
    quint32 export_idle_ms = 60000;
};
struct Mount {
    QUuid id;
    NodeId parent;
    dsfm::NamespaceId namespace_id;
    dsfm::DirectoryId source_directory;
    QString name;
    QJsonObject presentation;
    QUuid assignment;
};
struct EffectiveMount {
    Mount mount;
    MountDefinition definition;
    QVector<MountDefinition> contributors;
};
struct NodeInfo {
    NodeId id;
    NodeId parent;
    QString name;
    dsfm::EntryType type = dsfm::EntryType::Directory;
    dsfm::EntryStatus status = dsfm::EntryStatus::Available;
    quint64 size = 0;
    QDateTime modified;
    QByteArray sha256;
    dsfm::HashState hash_state = dsfm::HashState::Missing;
    quint64 revision = 0; // Derived from visible metadata, never backend counters.
    dsfm::Capabilities capabilities;
    dsfm::SymlinkPolicy symlinks = dsfm::SymlinkPolicy::Never;
};
using DirectoryPage = dsfm::Page<NodeInfo>;
struct PreparedRead {
    NodeInfo info;
    std::unique_ptr<dsfm::IFileReader> reader;
    dsfm::ReadConsistency consistency = dsfm::ReadConsistency::DetectChanges;
    QByteArray resume_lease;
    std::function<bool()> current; // Memory-only offer/retransmission admission.
};
struct Change {
    NodeId directory;
    QByteArray revision;
    bool invalidated = false;
};
struct ChangeCallback {
    QPointer<QObject> context;
    std::function<void(Change)> callback;
};
class Subscription {
  public:
    struct State;
    Subscription() = default;
    ~Subscription();
    Subscription(Subscription &&) noexcept;
    Subscription &operator=(Subscription &&) noexcept;
    Subscription(const Subscription &) = delete;
    void reset() noexcept;

  private:
    friend class Vfs;
    explicit Subscription(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
class CatalogExport {
  public:
    struct State;
    ~CatalogExport();
    quint64 size() const;
    QByteArray sha256() const;
    QUuid identity() const;
    QUuid authorizationScope() const;
    bool isAuthorized() const;
    bool requiresUtf8() const;
    QCoro::Task<Result<dsfm::PreparedRead>> prepareRead(dsfm::ReadPreparationOptions options = {});
    QCoro::Task<Result<void>> close();

  private:
    friend class Vfs;
    explicit CatalogExport(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
// One application-wide service, lightweight identity/mount records, shared
// backend registrations. All public operations run on this QObject's thread.
class Vfs final : public QObject {
    Q_OBJECT
  public:
    struct Impl;
    explicit Vfs(QString private_root, Limits limits = {}, QObject *parent = nullptr);
    ~Vfs() override;
    QCoro::Task<Result<void>> open();
    Result<void> registerNamespace(dsfm::NamespaceId ns, std::shared_ptr<dsfm::IFileManager> backend,
                                   dsfm::SymlinkPolicy symlinks = dsfm::SymlinkPolicy::Never,
                                   QUuid owner_identity = {});
    QCoro::Task<Result<StorageResource>> addResource(StorageResource resource, Request admin);
    QCoro::Task<Result<StorageAssignment>> assignStorage(StorageAssignment assignment, Request admin);
    QCoro::Task<Result<void>> withdrawAssignment(QUuid assignment, Request admin);
    Result<QVector<StorageResource>> resources() const;
    Result<QVector<StorageAssignment>> assignments(QUuid identity) const;
    QCoro::Task<Result<MountDefinition>> defineMount(MountDefinition definition, Request admin);
    QCoro::Task<Result<void>> relocateDefinition(QUuid definition, QString path, Request admin);
    QCoro::Task<Result<void>> removeDefinition(QUuid definition, Request admin);
    Result<QVector<MountDefinition>> definitions(QUuid identity) const;
    QCoro::Task<Result<Request>> compose(QUuid identity, Request request);
    Result<QVector<EffectiveMount>> effectiveMounts(QUuid identity, const Request &request) const;
    NodeId root(QUuid identity) const;
    Result<NodeId> nodeId(QUuid identity, QUuid mount, QUuid source) const;
    QCoro::Task<Result<Mount>> addMount(QUuid identity, Mount mount, Request request);
    QCoro::Task<Result<void>> changeMount(QUuid identity, QUuid mount, NodeId parent, QString name, Request request);
    QCoro::Task<Result<void>> removeMount(QUuid identity, QUuid mount, Request request);
    Result<QVector<Mount>> mounts(QUuid identity) const;
    QCoro::Task<Result<NodeInfo>> stat(QUuid identity, NodeId node, Request request);
    QCoro::Task<Result<NodeInfo>> lookup(QUuid identity, NodeId parent, QString name, Request request);
    QCoro::Task<Result<DirectoryPage>> list(QUuid identity, NodeId parent, dsfm::PageRequest page, Request request);
    QCoro::Task<Result<PreparedRead>> prepareRead(QUuid identity, NodeId node, dsfm::ReadPreparationOptions options,
                                                  Request request);
    QCoro::Task<Result<Subscription>> subscribeDirectory(QUuid identity, NodeId node, ChangeCallback callback,
                                                         Request request);
    QCoro::Task<Result<NodeInfo>> mkdir(QUuid identity, NodeId parent, QString name, Request request);
    QCoro::Task<Result<dsfm::PendingFile>> createFile(QUuid identity, NodeId parent, dsfm::CreateOptions options,
                                                      Request request);
    QCoro::Task<Result<dsfm::PendingFile>> replaceFile(QUuid identity, NodeId node, dsfm::ContentConstraints content,
                                                       Request request);
    QCoro::Task<Result<dsfm::MutationResult>> rename(QUuid identity, NodeId node, QString name, Request request);
    QCoro::Task<Result<dsfm::MutationResult>> move(QUuid identity, NodeId node, NodeId parent, Request request);
    QCoro::Task<Result<dsfm::MutationResult>> remove(QUuid identity, NodeId node, Request request);
    // The host supplies an application-private managed namespace. It is never
    // mounted or remotely addressable except through the returned export lease.
    Result<void> setExportStore(std::shared_ptr<dsfm::IFileManager> backend, dsfm::NamespaceId ns);
    QCoro::Task<Result<std::shared_ptr<CatalogExport>>> exportCatalog(QUuid identity, NodeId node, bool recursive,
                                                                      Request request, CancellationToken cancel = {});
    // Validate hostile export content before a consumer publishes its cache.
    QCoro::Task<Result<void>> validateCatalog(std::shared_ptr<dsfm::IFileReader> reader, quint64 size,
                                              QByteArray sha256, CancellationToken cancel = {});
    QCoro::Task<Result<void>> refresh();
    QCoro::Task<Result<void>> shutdown();
  signals:
    // Trusted local application notification only. Coalesced per mount/name.
    void conflictDetected(QUuid identity, QByteArray key, QString virtual_location);
    void conflictResolved(QUuid identity, QByteArray key);

  private:
    std::shared_ptr<Impl> impl_;
};
} // namespace dsvfs
