#pragma once

#include <QCoroTask>
#include <QByteArray>
#include <QDateTime>
#include <QObject>
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
enum class EntryStatus { Available, Missing, Inconsistent };
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
};
struct ByteRange {
    quint64 offset = 0;
    quint64 length = 0;
    bool operator==(const ByteRange&) const = default;
};
struct PageRequest { quint32 limit = 100; QByteArray cursor; };
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
    virtual QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype max_bytes, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<void>> close() = 0;
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
struct PendingFile {
    PendingInfo info;
    std::unique_ptr<IFileWriter> writer;
};
class IFileManager {
public:
    virtual ~IFileManager() = default;
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
    virtual QCoro::Task<Result<MutationResult>> rename(NamespaceId ns, QUuid id, QString name, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<MutationResult>> move(NamespaceId ns, QUuid id, DirectoryId destination, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<MutationResult>> remove(NamespaceId ns, QUuid id, CancellationToken cancel = {}) = 0;
    virtual QCoro::Task<Result<OperationInfo>> operationStatus(NamespaceId ns, OperationId id) = 0;
    virtual QCoro::Task<Result<ReconciliationPage>> reconcile(NamespaceId ns, ReconciliationRequest request, CancellationToken cancel = {}) = 0;
    virtual Result<quint64> subscribe(NamespaceId ns, QObject* context, std::function<void(Change)> callback) = 0;
    virtual void unsubscribe(quint64 subscription) = 0;
    virtual QCoro::Task<Result<void>> shutdown() = 0;
};

// Raw input is bounded before Unicode processing. No filesystem access.
Result<QString> normalizeName(QString name);
Result<QString> nameKey(QString name);

class NativeFileManager final : public IFileManager {
public:
    struct Impl;
    NativeFileManager();
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
    QCoro::Task<Result<MutationResult>> rename(NamespaceId ns, QUuid id, QString name, CancellationToken cancel = {}) override;
    QCoro::Task<Result<MutationResult>> move(NamespaceId ns, QUuid id, DirectoryId destination, CancellationToken cancel = {}) override;
    QCoro::Task<Result<MutationResult>> remove(NamespaceId ns, QUuid id, CancellationToken cancel = {}) override;
    QCoro::Task<Result<OperationInfo>> operationStatus(NamespaceId ns, OperationId id) override;
    QCoro::Task<Result<ReconciliationPage>> reconcile(NamespaceId ns, ReconciliationRequest request, CancellationToken cancel = {}) override;
    Result<quint64> subscribe(NamespaceId ns, QObject* context, std::function<void(Change)> callback) override;
    void unsubscribe(quint64 subscription) override;
    QCoro::Task<Result<void>> shutdown() override;
private:
    std::shared_ptr<Impl> impl_;
};
} // namespace dsfm
