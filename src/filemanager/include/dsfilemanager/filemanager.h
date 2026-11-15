#pragma once

#include <QCoroTask>
#include <QByteArray>
#include <QByteArrayView>
#include <QFlags>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QUuid>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace dsfm {
using NamespaceId = QUuid;
using FileId = QUuid;
using DirectoryId = QUuid;
using PendingId = QUuid;
using OperationId = QUuid;

enum class ErrorCode {
    MissingEntry, Conflict, InvalidName, InvalidArgument, PermissionDenied,
    StorageUnavailable, UnsupportedCapability, Corruption, IntegrityError,
    IncompleteContent, Cancelled, IoError, DiskFull, QuotaExceeded, QueueFull,
    Busy, NamespaceBusy, DirectoryNotEmpty, CrossNamespaceMove,
    ExternalModification, StaleCursor, IndeterminateOperation, ShuttingDown
};
struct Error {
    Error() = default;
    Error(ErrorCode value, int native = 0) : code(value), native_code(native) {}
    ErrorCode code = ErrorCode::IoError;
    int native_code = 0;
    quint64 bytes_completed = 0;
    OperationId operation_id;
};
template<typename T> class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : value_(std::move(error)) {}
    explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
    T& value() & { return std::get<T>(value_); }
    const T& value() const & { return std::get<T>(value_); }
    T&& value() && { return std::get<T>(std::move(value_)); }
    const Error& error() const { return std::get<Error>(value_); }
private:
    std::variant<T, Error> value_;
};
template<> class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}
    explicit operator bool() const noexcept { return !error_; }
    const Error& error() const { return error_.value(); }
private:
    std::optional<Error> error_;
};
class CancellationToken {
public:
    CancellationToken() : state_(std::make_shared<std::atomic_bool>(false)) {}
    void cancel() const noexcept { state_->store(true); }
    bool cancelled() const noexcept { return state_->load(); }
private:
    std::shared_ptr<std::atomic_bool> state_;
};
enum class PhysicalNaming { OriginalName, FileId };
enum class EntryType { File, Directory };
// Explicit values are persisted by the native catalog.
enum class EntryStatus { Available = 0, Missing = 1, Inconsistent = 2, Updating = 3 };
enum class HashState { Missing, Stale, Computing, Verified };
enum class NamespaceStatus { Updating, Ready, Failed };
enum class Capability : quint32 {
    Read = 1U << 0, Write = 1U << 1, PreparedRead = 1U << 2,
    DirectorySubscriptions = 1U << 3, SnapshotReads = 1U << 4, ResumeLeases = 1U << 5
};
Q_DECLARE_FLAGS(Capabilities, Capability)
Q_DECLARE_OPERATORS_FOR_FLAGS(Capabilities)
enum class ReadConsistency { DetectChanges, Snapshot };
constexpr bool isListable(EntryStatus status) noexcept {
    return status == EntryStatus::Available || status == EntryStatus::Updating;
}
constexpr bool isReadable(EntryStatus status) noexcept { return status == EntryStatus::Available; }
enum class ConflictPolicy { Fail, UniqueName };
enum class RangeKind { StorageValid, ApplicationVerified };
enum class PendingState { Ready, Suspended, IntegrityFailed, Inconsistent };
enum class Durability { Synchronized, SyncFailed };
struct NamespaceLimits {
    quint64 max_file_size = 16ULL << 30;
    quint64 max_total_bytes = 64ULL << 30;
    quint64 max_staging_bytes = 16ULL << 30;
    quint64 max_catalog_bytes = 64ULL << 20;
    quint32 max_pending = 64;
    quint32 max_writers = 16;
    quint32 max_handles = 128;
    quint32 max_entries = 100000;
    quint32 max_depth = 64;
    quint32 max_ranges = 4096;
    quint32 max_page_size = 100;
    quint32 max_operations = 128;
    quint32 max_io_bytes = 1U << 20;
    quint64 max_buffered_bytes = 8ULL << 20;
    quint32 max_collision_attempts = 100;
    quint32 max_notifications = 128;
    quint32 max_batch = 100;
};
struct NamespaceConfig {
    NamespaceId id;
    QString storage_root;
    PhysicalNaming naming = PhysicalNaming::FileId;
    NamespaceLimits limits;
    std::optional<NamespaceLimits> limits_override; // Explicit override on reopen; otherwise use persisted limits.
    bool repair_permissions = false; // Local runtime policy: tighten owned entries on access; never change ownership.
};
struct NamespaceInfo {
    NamespaceConfig config;
    DirectoryId root;
    quint64 generation = 0;
    quint64 charged_bytes = 0;
    bool retain_readers_on_replace = false;
    bool durable_sync = true;
    quint64 untracked_staging = 0; // Minimum count from a bounded scan.
    bool staging_scan_truncated = false;
    NamespaceStatus status = NamespaceStatus::Updating;
    Capabilities capabilities;
    std::optional<Error> readiness_error;
};
struct ContentConstraints {
    std::optional<quint64> expected_size;
    QByteArray expected_sha256;
};
struct CreateOptions {
    DirectoryId parent;
    QString suggested_name;
    ConflictPolicy conflict = ConflictPolicy::UniqueName;
    ContentConstraints content;
};
struct FileInfo {
    FileId id;
    std::optional<DirectoryId> parent;
    QString name;
    EntryType type = EntryType::File;
    EntryStatus status = EntryStatus::Available;
    quint64 size = 0;
    QDateTime created;
    QDateTime modified;
    QByteArray sha256;
    quint64 generation = 0;
    HashState hash_state = HashState::Missing;
    // Identifies the backing namespace after cross-mount alias delegation.
    NamespaceId namespace_id;
};
struct ByteRange {
    quint64 offset = 0;
    quint64 length = 0;
    bool operator==(const ByteRange&) const = default;
};
struct PageRequest {
    quint32 limit = 100; QByteArray cursor;
    // Live keyset enumeration for composition. Normal browsing retains its
    // generation-consistent default. Never forward backend cursors to peers.
    bool stable_order = false;
    bool ascii_order = false;
};
template<typename T> struct Page {
    QVector<T> entries;
    QByteArray next_cursor;
};
using DirectoryPage = Page<FileInfo>;
using RangePage = Page<ByteRange>;
struct PendingInfo {
    PendingId id;
    FileId file_id;
    DirectoryId parent;
    QString name;
    ContentConstraints content;
    std::optional<quint64> final_size;
    PendingState state = PendingState::Ready;
    bool replacement = false;
    quint64 range_generation = 0;
};
using PendingPage = Page<PendingInfo>;
struct MutationResult {
    OperationId operation_id;
    quint64 generation = 0;
    Durability durability = Durability::Synchronized;
};
using CommitResult = MutationResult;
struct OperationInfo {
    OperationId id;
    QString phase;
    QString kind;
    FileId file_id;
    std::optional<MutationResult> result;
};
struct Change {
    NamespaceId namespace_id;
    quint64 generation = 0;
    bool rescan_required = true;
};
enum class ReconciliationAction { Scan, RetryValidation, DiscardMissing };
struct ReconciliationRequest {
    ReconciliationAction action = ReconciliationAction::Scan;
    FileId id;
    PageRequest page;
};
using ReconciliationPage = Page<FileInfo>;

class IFileReader {
public:
    virtual ~IFileReader() = default;
    // Memory-only admission for queued offers/retransmissions. The returned
    // guard may outlive the descriptor; it is not filesystem validation or a
    // capability grant. Actual access still requires preparation/read checks.
    virtual std::function<bool()> accessGuard() const { return [] { return true; }; }
    virtual QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype max_bytes, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<void>> close() = 0;
    // Report a digest mismatch in bytes read from this opened object. Backends
    // invalidate its cached digest/version; this never permits content mutation.
    virtual QCoro::Task<Result<void>> invalidate() { co_return Error{ErrorCode::UnsupportedCapability}; }
};
class IFileWriter {
public:
    virtual ~IFileWriter() = default;
    virtual QCoro::Task<Result<qsizetype>> writeAt(quint64 offset, QByteArray data, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<void>> setFinalSize(quint64 size) = 0;
    virtual QCoro::Task<Result<void>> markVerified(ByteRange range) = 0;
    virtual QCoro::Task<Result<void>> checkpoint(CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<void>> suspend(CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<CommitResult>> commit(CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<void>> abort(CancellationToken cancel = {}) = 0;
};
// Optional requirements bind the returned reader to the requested committed version.
struct ReadPreparationOptions {
    std::optional<quint64> expected_generation;
    QByteArray expected_sha256;
    CancellationToken cancel;
    // Opaque backend lease; resumption also requires the original expected digest.
    QByteArray resume_lease;
};
struct PreparedRead {
    FileInfo info;
    std::unique_ptr<IFileReader> reader;
    ReadConsistency consistency = ReadConsistency::DetectChanges;
    QByteArray resume_lease;
};
enum class DirectoryChangeKind { RescanRequired, Invalidated };
struct DirectoryChange {
    NamespaceId namespace_id;
    DirectoryId directory_id;
    quint64 generation = 0;
    DirectoryChangeKind kind = DirectoryChangeKind::RescanRequired;
};
// Register from context's thread. Delivery always uses that Qt execution context.
struct DirectoryChangeCallback {
    QPointer<QObject> context;
    std::function<void(DirectoryChange)> callback;
};
class DirectorySubscription {
public:
    DirectorySubscription() = default;
    ~DirectorySubscription();
    DirectorySubscription(const DirectorySubscription&) = delete;
    DirectorySubscription& operator=(const DirectorySubscription&) = delete;
    DirectorySubscription(DirectorySubscription&& other) noexcept;
    DirectorySubscription& operator=(DirectorySubscription&& other) noexcept;
    void reset() noexcept;
    explicit operator bool() const noexcept;
    NamespaceId namespaceId() const noexcept { return namespace_id_; }
    DirectoryId directoryId() const noexcept { return directory_id_; }
    quint64 generation() const noexcept { return generation_; }
    // Backend construction hook; release must not throw or block. Cancellation is
    // signalled through active before release is invoked.
    DirectorySubscription(NamespaceId ns, DirectoryId directory, quint64 generation,
        std::shared_ptr<std::atomic_bool> active, std::function<void()> release);
private:
    NamespaceId namespace_id_;
    DirectoryId directory_id_;
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> active_;
    std::function<void()> release_;
};
struct PendingFile {
    PendingInfo info;
    std::unique_ptr<IFileWriter> writer;
};
// Request state is immutable except for the host's shared policy-generation counter.
enum class AccessOperation { Inspect, List, Read };
struct FilesystemIdentity {
    quint64 device = 0;
    quint64 inode = 0;
    bool directory = false;
};
struct EntryTarget { NamespaceId namespace_id; QUuid id; };
class IFileManager;
struct AccessContext {
    // Stable opaque requester/session scope; required for authorized pagination.
    QUuid authorization_scope;
    quint64 generation = 0;
    std::shared_ptr<const std::atomic<quint64>> current_generation;
    std::function<Result<bool>(NamespaceId, QUuid, AccessOperation)> check;
    // Memory-only VFS lookup. Never perform reentrant storage I/O in callbacks.
    std::function<Result<EntryTarget>(FilesystemIdentity)> resolve_target;
    // Optional mount-aware context for a resolved directory destination. The
    // delegating backend carries its projected filesystem guard into this
    // context. This disambiguates descendants of overlapping mounted aliases.
    std::function<Result<std::shared_ptr<AccessContext>>(EntryTarget)> target_context;
    // Additional exclusion predicates supplied by composition. Backends apply
    // these to opened objects and every traversed ancestor; unsupported backends
    // fail closed rather than drop the predicate when delegating an alias.
    std::function<bool(FilesystemIdentity)> filesystem_check;
    QByteArray filesystem_generation;
    std::function<std::shared_ptr<IFileManager>(NamespaceId)> backend;
    DirectoryId assignment_directory;
    bool allow_external_files = false; // administrative assignment capability
    quint32 remaining_aliases = 32;
    bool current() const noexcept {
        return !current_generation || current_generation->load() == generation;
    }
};
class IFileManager {
public:
    IFileManager();
    virtual ~IFileManager();
    virtual QCoro::Task<Result<NamespaceInfo>> createNamespace(NamespaceConfig config, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<NamespaceInfo>> openNamespace(NamespaceConfig config, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<NamespaceInfo>> namespaceInfo(NamespaceId ns) = 0;
    virtual QCoro::Task<Result<QVector<NamespaceInfo>>> namespaces() = 0;
    virtual QCoro::Task<Result<FileInfo>> stat(NamespaceId ns, QUuid id) = 0;
    virtual QCoro::Task<Result<FileInfo>> lookup(NamespaceId ns, QString logical_path) = 0;
    virtual QCoro::Task<Result<DirectoryPage>> list(NamespaceId ns, DirectoryId parent, PageRequest page = {}) = 0;
    virtual QCoro::Task<Result<DirectoryId>> mkdir(NamespaceId ns, DirectoryId parent, QString name) = 0;
    virtual QCoro::Task<Result<PendingFile>> createFile(NamespaceId ns, CreateOptions options) = 0;
    virtual QCoro::Task<Result<PendingFile>> replaceFile(NamespaceId ns, FileId id, ContentConstraints content) = 0;
    virtual QCoro::Task<Result<PendingFile>> openPending(NamespaceId ns, PendingId id) = 0;
    virtual QCoro::Task<Result<void>> abortPending(NamespaceId ns, PendingId id, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<PendingPage>> listPending(NamespaceId ns, PageRequest page = {}) = 0;
    virtual QCoro::Task<Result<RangePage>> listRanges(NamespaceId ns, PendingId id, RangeKind kind, PageRequest page = {}) = 0;
    virtual QCoro::Task<Result<std::unique_ptr<IFileReader>>> openRead(NamespaceId ns, FileId id) = 0;
    virtual QCoro::Task<Result<PreparedRead>> prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options = {});
    virtual QCoro::Task<Result<DirectorySubscription>> subscribeDirectory(NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback);
    virtual QCoro::Task<Result<MutationResult>> rename(NamespaceId ns, QUuid id, QString name, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<MutationResult>> move(NamespaceId ns, QUuid id, DirectoryId destination, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<MutationResult>> remove(NamespaceId ns, QUuid id, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<OperationInfo>> operationStatus(NamespaceId ns, OperationId id) = 0;
    virtual QCoro::Task<Result<ReconciliationPage>> reconcile(NamespaceId ns, ReconciliationRequest request, CancellationToken cancel = {}) = 0;
    virtual Result<quint64> subscribe(NamespaceId ns, QObject* context, std::function<void(Change)> callback) = 0;
    virtual void unsubscribe(quint64 subscription) = 0;
    // Contextual operations preserve the single storage interface. Native entries
    // use these defaults; alias-aware backends override them.
    virtual QCoro::Task<Result<FileInfo>> stat(NamespaceId ns, QUuid id, AccessContext access);
    virtual QCoro::Task<Result<FileInfo>> lookup(NamespaceId ns, QString path, AccessContext access);
    virtual QCoro::Task<Result<DirectoryPage>> list(NamespaceId ns, DirectoryId parent, PageRequest page, AccessContext access);
    virtual QCoro::Task<Result<std::unique_ptr<IFileReader>>> openRead(NamespaceId ns, FileId id, AccessContext access);
    virtual QCoro::Task<Result<PreparedRead>> prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options, AccessContext access);
    virtual QCoro::Task<Result<DirectorySubscription>> subscribeDirectory(NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback, AccessContext access);
    // Composition retains this context while representing directory aliases.
    // Privacy-aware backends project their mandatory exclusions/revocation into
    // it so a different backend cannot bypass them when serving descendants.
    virtual bool supportsStableEnumeration() const noexcept { return false; }
    virtual QCoro::Task<Result<void>> discardDirectoryCursor(NamespaceId ns, QByteArray cursor);
    virtual QCoro::Task<Result<AccessContext>> descendantAccess(NamespaceId ns, DirectoryId directory, AccessContext access);
    virtual QCoro::Task<Result<void>> shutdown() = 0;
protected:
    using FilesystemGuard = std::function<QCoro::Task<Result<void>>() >;
    virtual FilesystemGuard filesystemGuard(NamespaceId ns, QUuid id, std::function<bool(FilesystemIdentity)> check);
private:
    struct AccessPages;
    std::shared_ptr<AccessPages> access_pages_;
};

// Raw input is bounded before Unicode processing. No filesystem access.
Result<QString> normalizeName(QString name);
Result<QString> nameKey(QString name);

// Linux source-name encoding v1: raw component bytes, no filesystem access.
// Preserves printable UTF-8, case and normalization; escapes unsafe bytes and '%'.
// Encoded names are logical labels, never URL-decoded to construct OS paths.
inline constexpr quint32 linux_name_encoding_version = 1;
inline constexpr qsizetype max_linux_name_bytes = 255;
inline constexpr qsizetype max_encoded_linux_name_bytes = 3 * max_linux_name_bytes;
Result<QString> encodeLinuxName(QByteArrayView name);

class NativeFileManager final : public IFileManager {
public:
    struct Impl;
    using IFileManager::stat;
    using IFileManager::lookup;
    using IFileManager::list;
    using IFileManager::openRead;
    using IFileManager::prepareRead;
    using IFileManager::subscribeDirectory;
    bool supportsStableEnumeration() const noexcept override { return true; }
    NativeFileManager();
    // Host privacy policies must also exclude this shared backend metadata root.
    static QString lockRegistryRoot();
    ~NativeFileManager() override;
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
protected:
    FilesystemGuard filesystemGuard(NamespaceId ns, QUuid id, std::function<bool(FilesystemIdentity)> check) override;
private:
    std::shared_ptr<Impl> impl_;
};
} // namespace dsfm
