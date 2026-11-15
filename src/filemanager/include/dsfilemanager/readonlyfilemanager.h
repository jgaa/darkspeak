#pragma once
#include <dsfilemanager/filemanager.h>

namespace dsfm {
enum class SymlinkPolicy { Never, WithinVfs, Always };
enum class ReadVerification { Cached, Rehash, Snapshot };
struct ExclusionPolicy {
    quint64 generation = 1;
    QStringList directory_roots;
    QStringList objects;
};
struct ReadOnlyLimits {
    quint32 max_scan_directories = 1024;
    quint32 max_scan_retries = 3;
    quint32 max_watches = 256;
    quint32 max_namespaces = 32;
    quint32 max_symlinks = 32;
    quint32 max_cursors = 128;
    quint32 max_cached_directories = 128;
    quint32 reconcile_interval_ms = 30000;
    quint32 reader_lifetime_ms = 300000;
};
struct ReadOnlyNamespaceConfig {
    NamespaceConfig common;
    SymlinkPolicy symlinks = SymlinkPolicy::Never;
    ReadVerification verification = ReadVerification::Cached;
    ExclusionPolicy exclusions;
    ReadOnlyLimits limits;
};
class ReadOnlyFileManager final : public IFileManager {
public:
    struct Impl;
    explicit ReadOnlyFileManager(QString private_root);
    ~ReadOnlyFileManager() override;
    QCoro::Task<Result<NamespaceInfo>> createNamespace(NamespaceConfig config, CancellationToken cancel = {}) override;
    QCoro::Task<Result<NamespaceInfo>> openNamespace(NamespaceConfig config, CancellationToken cancel = {}) override;
    QCoro::Task<Result<NamespaceInfo>> namespaceInfo(NamespaceId ns) override;
    QCoro::Task<Result<QVector<NamespaceInfo>>> namespaces() override;
    QCoro::Task<Result<FileInfo>> stat(NamespaceId ns, QUuid id) override;
    QCoro::Task<Result<FileInfo>> lookup(NamespaceId ns, QString logical_path) override;
    QCoro::Task<Result<DirectoryPage>> list(NamespaceId ns, DirectoryId parent, PageRequest page = {}) override;
    QCoro::Task<Result<DirectoryId>> mkdir(NamespaceId ns, DirectoryId parent, QString name) override;
    QCoro::Task<Result<PendingFile>> createFile(NamespaceId ns, CreateOptions options) override;
    QCoro::Task<Result<PendingFile>> replaceFile(NamespaceId ns, FileId id, ContentConstraints content) override;
    QCoro::Task<Result<PendingFile>> openPending(NamespaceId ns, PendingId id) override;
    QCoro::Task<Result<void>> abortPending(NamespaceId ns, PendingId id, CancellationToken cancel = {}) override;
    QCoro::Task<Result<PendingPage>> listPending(NamespaceId ns, PageRequest page = {}) override;
    QCoro::Task<Result<RangePage>> listRanges(NamespaceId ns, PendingId id, RangeKind kind, PageRequest page = {}) override;
    QCoro::Task<Result<std::unique_ptr<IFileReader>>> openRead(NamespaceId ns, FileId id) override;
    QCoro::Task<Result<PreparedRead>> prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options = {}) override;
    QCoro::Task<Result<DirectorySubscription>> subscribeDirectory(NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback) override;
    QCoro::Task<Result<MutationResult>> rename(NamespaceId ns, QUuid id, QString name, CancellationToken cancel = {}) override;
    QCoro::Task<Result<MutationResult>> move(NamespaceId ns, QUuid id, DirectoryId destination, CancellationToken cancel = {}) override;
    QCoro::Task<Result<MutationResult>> remove(NamespaceId ns, QUuid id, CancellationToken cancel = {}) override;
    QCoro::Task<Result<OperationInfo>> operationStatus(NamespaceId ns, OperationId id) override;
    QCoro::Task<Result<ReconciliationPage>> reconcile(NamespaceId ns, ReconciliationRequest request, CancellationToken cancel = {}) override;
    Result<quint64> subscribe(NamespaceId ns, QObject* context, std::function<void(Change)> callback) override;
    void unsubscribe(quint64 subscription) override;
    QCoro::Task<Result<void>> shutdown() override;
    QCoro::Task<Result<NamespaceInfo>> createSource(ReadOnlyNamespaceConfig config, CancellationToken cancel = {});
    QCoro::Task<Result<NamespaceInfo>> openSource(ReadOnlyNamespaceConfig config, CancellationToken cancel = {});
    // Offline rebuild: preserves one owner-only catalog directory backup; never touches source data.
    QCoro::Task<Result<NamespaceInfo>> reindexSource(ReadOnlyNamespaceConfig config, CancellationToken cancel = {});
    QCoro::Task<Result<void>> updateExclusions(NamespaceId ns, ExclusionPolicy policy);
    QCoro::Task<Result<void>> closeSource(NamespaceId ns);
    QCoro::Task<Result<ReadOnlyNamespaceConfig>> sourceConfig(NamespaceId ns);
    QCoro::Task<Result<FileInfo>> stat(NamespaceId ns, QUuid id, AccessContext access) override;
    QCoro::Task<Result<FileInfo>> lookup(NamespaceId ns, QString path, AccessContext access) override;
    QCoro::Task<Result<DirectoryPage>> list(NamespaceId ns, DirectoryId parent, PageRequest page, AccessContext access) override;
    QCoro::Task<Result<std::unique_ptr<IFileReader>>> openRead(NamespaceId ns, FileId id, AccessContext access) override;
    QCoro::Task<Result<PreparedRead>> prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options, AccessContext access) override;
    QCoro::Task<Result<DirectorySubscription>> subscribeDirectory(NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback, AccessContext access) override;
    bool supportsStableEnumeration() const noexcept override { return true; }
    QCoro::Task<Result<void>> discardDirectoryCursor(NamespaceId ns, QByteArray cursor) override;
    QCoro::Task<Result<AccessContext>> descendantAccess(NamespaceId ns, DirectoryId directory, AccessContext access) override;
private:
    QCoro::Task<Result<FileInfo>> statImpl(NamespaceId ns, QUuid id, AccessContext access);
    QCoro::Task<Result<FileInfo>> lookupImpl(NamespaceId ns, QString path, AccessContext access);
    QCoro::Task<Result<DirectoryPage>> listImpl(NamespaceId ns, DirectoryId parent, PageRequest page, AccessContext access);
    QCoro::Task<Result<std::unique_ptr<IFileReader>>> openReadImpl(NamespaceId ns, FileId id, AccessContext access);
    QCoro::Task<Result<PreparedRead>> prepareReadImpl(NamespaceId ns, FileId id, ReadPreparationOptions options, AccessContext access, bool delegate_alias = true);
    std::shared_ptr<Impl> impl_;
};
} // namespace dsfm
