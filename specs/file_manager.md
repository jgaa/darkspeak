## 1. Scope and architecture

Implement a standalone, reusable C++20 FileManager library providing a virtual filesystem independent of physical storage. First delivery: public headers, install/export CMake targets, a native filesystem backend, tests, and a minimal example. DarkSpeak application migration is a separate task. Application configuration and internal application data are outside the managed filesystem.

Use Qt types, `QCoro::Task<T>`, and a library-defined `Result<T>` with typed errors. Follow `agents.md` coding conventions, including snake_case public data members and variables and trailing underscores for private members. Components: `IFileManager`, `IFileReader`, `IFileWriter`, move-only `DirectorySubscription`, metadata/result types, `NativeFileManager`, and a bounded I/O executor. No dependencies on DarkSpeak identities, protocol, UI, or network code. Keep public contracts storage-independent; the native backend is the first implementation and the read-only backend is specified separately. Maintain **one** abstract storage interface (`IFileManager`): add generally useful operations there, with default `UnsupportedCapability` implementations for optional methods; backend-specific configuration/diagnostics may be accessed by casting to a concrete type at administration boundaries. Ordinary callers must not cast.

## 2. Identity and namespaces

```cpp
using NamespaceId = QUuid;
using FileId = QUuid;
using DirectoryId = QUuid;
using PendingId = QUuid;
using OperationId = QUuid;

enum class PhysicalNaming { OriginalName, FileId };

struct NamespaceConfig {
    NamespaceId id;
    QString storage_root; // Native backend configuration, never a peer path.
    PhysicalNaming naming;
    NamespaceLimits limits;
};
```

IDs are locally generated, non-null UUIDs; validate namespace membership on every lookup. Paths are never primary identifiers. IDs survive restarts, rename, move, and replacement. Each namespace has one persisted root `DirectoryId` with no parent; ordinary entries have exactly one directory parent. The root cannot be renamed, moved, replaced, or removed through entry operations. Namespace deletion is outside initial scope.

Each namespace has independent configuration, catalog, limits, and physical storage:

- **OriginalName:** normalized, conflict-resolved logical names and directory hierarchy are mirrored on disk for OS browsing.
- **FileId:** physical files use opaque UUID names without original extensions; logical names and directories exist in the catalog.

Logical names and API behavior are independent of naming policy. Persist the policy; reject changes on populated namespaces until an explicit migration exists. Namespace reopening validates configuration and on-disk identity rather than silently creating a new catalog or adopting contents.

Reject identical or ancestor/descendant storage roots, including aliases resolvable through filesystem identity. Within a process, check all opened roots; across processes, acquire namespace and ancestor/descendant exclusion locks under a documented platform locking protocol before creation/opening. Reject storage where this exclusion cannot be enforced. Root ownership checks and overlap exclusion must precede catalog creation.

Allow one owning process per namespace, enforced by an explicit process lock held through recovery, operation, and shutdown. A second owner receives `NamespaceBusy`; a PID file alone is insufficient. Recover stale ownership only using a protocol that cannot steal a live owner's lock. Holding a lock does not grant protection against arbitrary external edits.

## 3. Metadata, constraints, and limits

```cpp
enum class EntryType { File, Directory };
enum class EntryStatus { Available = 0, Missing = 1, Inconsistent = 2, Updating = 3 };
enum class HashState { Missing, Stale, Computing, Verified };
enum class NamespaceStatus { Updating, Ready, Failed };
enum class ReadConsistency { DetectChanges, Snapshot };
enum class ConflictPolicy { Fail, UniqueName };

struct FileInfo {
    FileId id; // Also represents directory entry IDs when type == Directory.
    std::optional<DirectoryId> parent;
    QString name;
    EntryType type;
    EntryStatus status;
    quint64 size;
    QDateTime created;
    QDateTime modified;
    QByteArray sha256; // Empty or exactly 32 bytes; consult hash_state before use.
    HashState hash_state;
    quint64 generation;
    NamespaceId namespace_id; // Backing namespace, including delegated aliases.
};

struct ContentConstraints {
    std::optional<quint64> expected_size;
    QByteArray expected_sha256; // Empty or exactly 32 bytes.
};

struct CreateOptions {
    DirectoryId parent;
    QString suggested_name;
    ConflictPolicy conflict = ConflictPolicy::UniqueName;
    ContentConstraints content;
};

struct ByteRange {
    quint64 offset;
    quint64 length; // Checked half-open interval [offset, offset + length).
};
```

`createFile()` always allocates a new logical file ID; it never replaces an existing ID. `replaceFile(FileId, ContentConstraints)` creates pending content for that existing ID and preserves its name, parent, and identity on commit. Names are reserved atomically against completed entries and pending creates. Failed/aborted creates release reservations only after recovery-safe cleanup. `Replace` is not a create conflict policy.

`NamespaceLimits` supplies finite, documented defaults and locally configurable maxima for:

- File logical size, total charged storage, and staging storage bytes.
- Persistent pending writes, active writer handles, reader handles, and total open handles.
- Completed/reserved entries, directory depth, and stored range records per pending file.
- Listing/range page size, queued/running operations, per-operation read/write bytes, and total buffered bytes.
- Filename collision attempts, notification backlog, and recovery/reconciliation batch work.

Enforce limits before allocation, reservation, queue admission, file extension, or durable-state growth. Validate input lengths before normalization/copying. Check all additions, range endpoints, offsets, and conversions; reject values exceeding signed backend offset limits even though the API uses `quint64`. Negative read lengths are invalid. Zero-length reads/writes do not extend files or create range records.

Reserve known expected sizes at admission. Unknown-size writers reserve additional capacity before extending; they still obey finite file/staging/total limits. Charge pending content, replacement overlap, retained reader versions, and cleanup backlog without double-counting the same physical version. Bound catalog/journal growth as well as payloads; document accounting units and filesystem overhead allowance. Quotas are admission controls, not guarantees against external disk consumption or disk-full errors. Preserve quota headroom for recovery and deletion; reducing limits below current usage blocks growth rather than deleting data.

## 4. Pending writes and asynchronous handles

```cpp
class IFileReader {
public:
    virtual ~IFileReader() = default;
    virtual QCoro::Task<Result<QByteArray>> readAt(
        quint64 offset, qsizetype max_bytes) = 0;
    virtual QCoro::Task<Result<void>> close() = 0;
};

class IFileWriter {
public:
    virtual ~IFileWriter() = default;
    virtual QCoro::Task<Result<qsizetype>> writeAt(
        quint64 offset, QByteArray data) = 0;
    virtual QCoro::Task<Result<void>> setFinalSize(quint64 size) = 0;
    virtual QCoro::Task<Result<void>> markVerified(ByteRange range) = 0;
    virtual QCoro::Task<Result<void>> checkpoint() = 0;
    virtual QCoro::Task<Result<void>> suspend() = 0;
    virtual QCoro::Task<Result<CommitResult>> commit() = 0;
    virtual QCoro::Task<Result<void>> abort() = 0;
};
```

`PendingFile` contains `PendingId`, reserved `FileId`, assigned logical name, persisted constraints, and an owned writer handle. `PendingInfo` additionally reports create/replace mode, state, final size if known, and range-generation information; range sets are retrieved through bounded paginated queries, never an unbounded metadata response.

Pending writes persist across restart. `openPending()` acquires exclusive writer ownership of an existing pending ID; `listPending()` returns deterministic paginated metadata, including suspended, inconsistent, and integrity-failed writes. Normal listings and `openRead()` never expose new pending content. A replacement leaves the previous committed version readable until publication.

Only one nonterminal pending replacement may reserve a logical file, and only one writer handle may own a pending write. Reject conflicting rename, move, remove, or replacement while a writer is active or its pending reservation remains; reject ancestor mutations that would invalidate such reservations. Suspended reservations require explicit commit or abort before conflicting mutations. Independent readers remain allowed.

Operations on each handle are serialized in admission order, including close/checkpoint/commit/abort. Concurrent calls do not execute overlapping writes. Data buffers and operation state are owned across suspension; read results never reference worker-owned scratch storage. Report short reads/writes precisely; only successfully written bytes become covered. Reads beyond EOF return empty data.

### 4.1 Durable ranges and lifecycle

Maintain separate normalized, nonoverlapping range sets:

- **Storage-valid:** bytes written and included in a successful durable checkpoint. This means persisted coverage, not proof against later external alteration.
- **Application-verified:** storage-valid bytes explicitly acknowledged by the caller through `markVerified()`, for example after a transfer-level chunk check. The library does not infer peer authentication or chunk verification.

Track dirty coverage separately until checkpoint. Persist range maps, constraints, final size, and generation atomically with safe ordering: synchronize data before recording durable coverage. Writes invalidate application-verified coverage over affected ranges; reject a write before changing bytes if its range-map growth cannot be represented within limits. For overwrites of checkpointed data, durably invalidate affected coverage before modifying it, or use copy-on-write with equivalent crash guarantees. A crash must never leave old verified flags attached to newly written or partially overwritten bytes.

`markVerified()` accepts only fully written ranges, checkpoints covered dirty bytes before persisting verification flags, and rejects gaps or out-of-bounds ranges. It does not calculate an application checksum. Persist expected constraints immutably for the pending lifetime; repair changes bytes, not the expected digest.

- `checkpoint()` synchronizes data and range state without publishing or releasing ownership. Successful writes alone need not survive a crash; report checkpoint durability failures explicitly.
- `suspend()` checkpoints and releases the writer handle while retaining the pending ID, data, constraints, and reservations. Failure leaves the handle usable and reports the error.
- `setFinalSize()` establishes the exact target size when not supplied initially; reject contradiction of expected size or existing bytes beyond the target. Do not infer completion from sparse-file length or the highest write offset.
- `commit()` performs a final checkpoint, requires a known final size and complete storage-valid coverage of `[0, final_size)`, verifies integrity, and publishes. Empty files are valid when size and checksum constraints permit. Application-verified ranges are exposed for resume decisions; they do not replace whole-file checksum verification.
- `abort()` durably marks the pending write aborted, prevents publication, and reclaims staging/reservations through idempotent cleanup. Aborting a replacement never deletes its previous committed version.
- Destroying an uncommitted writer never publishes or implicitly aborts. Release ownership safely; retain recoverable checkpointed state. Asynchronous shutdown checkpoints live writers when possible and reports failures. Recovery discards uncheckpointed coverage, not all pending files.

Reopening validates staging identity, range bounds, constraints, and generation. Missing/uncertain staging is flagged, never silently considered complete. Persisted application-verified ranges describe prior caller verification, not current end-to-end integrity proof.

### Integrity verification

The FileManager supports an optional expected SHA-256 checksum. DarkSpeak requires this checksum for every incoming file.

- A file with an expected checksum must never become available until that checksum has been verified. During replacement, the previous committed version may remain available; pending replacement content may not.
- For uninterrupted sequential writes, the implementation may calculate SHA-256 incrementally during the transfer, avoiding a second read, provided the hash covers exactly the final bytes and staging cannot change undetected before publication.
- For resumed transfers, recalculate SHA-256 over the **entire completed file**, including previously received data, before commit.
- Random-access writes, overwrites, or other operations that invalidate incremental hashing require complete recalculation.
- `commit()` verifies all required byte ranges are present and the calculated checksum matches the expected checksum. Reject malformed expected digests at admission.
- Checksum failure leaves content unpublished and returns `IntegrityError`. The pending file remains unavailable to normal readers until repaired and successfully committed, or aborted; persist its failure state for inspection/resume.
- Verification runs in bounded chunks off the UI thread and respects cancellation and crash-recovery guarantees. Prevent concurrent writes during verification/publication; if backing data cannot be trusted to remain stable, fail rather than publish a stale verification result.
- Never trust a previously persisted checksum calculation or serialized hash state as proof of integrity after reopening a pending file. Recovery that must finish publication after a crash must reverify the complete candidate before making it available through the API.

The reusable library may permit files without expected checksums; completeness and size checks still apply. DarkSpeak must always supply an expected checksum for incoming transfers. Application-verified ranges do not relax this requirement.

## 5. Virtual filesystem interface

Public API sketch; supporting structs and cancellation parameters must be defined in the public headers with the contracts below.

```cpp
class IFileManager {
public:
    virtual ~IFileManager() = default;
    virtual QCoro::Task<Result<NamespaceInfo>> createNamespace(
        NamespaceConfig config) = 0;
    virtual QCoro::Task<Result<NamespaceInfo>> openNamespace(
        NamespaceConfig config) = 0;
    virtual QCoro::Task<Result<NamespaceInfo>> namespaceInfo(NamespaceId ns) = 0;
    virtual QCoro::Task<Result<FileInfo>> stat(NamespaceId ns, QUuid id) = 0;
    virtual QCoro::Task<Result<FileInfo>> lookup(
        NamespaceId ns, QString logical_path) = 0;
    virtual QCoro::Task<Result<DirectoryPage>> list(
        NamespaceId ns, DirectoryId parent, PageRequest page) = 0;
    virtual QCoro::Task<Result<DirectoryId>> mkdir(
        NamespaceId ns, DirectoryId parent, QString name) = 0;
    virtual QCoro::Task<Result<PendingFile>> createFile(
        NamespaceId ns, CreateOptions options) = 0;
    virtual QCoro::Task<Result<PendingFile>> replaceFile(
        NamespaceId ns, FileId id, ContentConstraints content) = 0;
    virtual QCoro::Task<Result<PendingFile>> openPending(
        NamespaceId ns, PendingId id) = 0;
    virtual QCoro::Task<Result<PendingPage>> listPending(
        NamespaceId ns, PageRequest page) = 0;
    virtual QCoro::Task<Result<RangePage>> listRanges(
        NamespaceId ns, PendingId id, RangeKind kind, PageRequest page) = 0;
    virtual QCoro::Task<Result<std::unique_ptr<IFileReader>>> openRead(
        NamespaceId ns, FileId id) = 0;
    // Optional common operation: default returns UnsupportedCapability.
    virtual QCoro::Task<Result<PreparedRead>> prepareRead(
        NamespaceId ns, FileId id, ReadPreparationOptions options);
    // Optional common operation: default returns UnsupportedCapability.
    virtual QCoro::Task<Result<DirectorySubscription>> subscribeDirectory(
        NamespaceId ns, DirectoryId directory,
        DirectoryChangeCallback callback);
    virtual QCoro::Task<Result<MutationResult>> rename(
        NamespaceId ns, QUuid id, QString name) = 0;
    virtual QCoro::Task<Result<MutationResult>> move(
        NamespaceId ns, QUuid id, DirectoryId destination) = 0;
    virtual QCoro::Task<Result<MutationResult>> remove(
        NamespaceId ns, QUuid id) = 0;
    virtual QCoro::Task<Result<OperationInfo>> operationStatus(
        NamespaceId ns, OperationId id) = 0;
    virtual QCoro::Task<Result<ReconciliationPage>> reconcile(
        NamespaceId ns, ReconciliationRequest request) = 0;
    virtual QCoro::Task<Result<void>> shutdown() = 0;
};
```

`NamespaceInfo` returns persisted configuration, root ID, limits/usage, supported visibility/durability capabilities, `NamespaceStatus status`, optional `readiness_error`, and common `Capabilities` flags (`Read`, `Write`, `PreparedRead`, `DirectorySubscriptions`, `SnapshotReads`, `ResumeLeases`). Report only implemented capabilities. The managed native backend reports `Ready` after recovery and supports the first four flags; it does not advertise snapshots or retained resume leases. Metadata validation and hash availability are distinct: an externally indexed `Available` file may have `HashState::Missing` until preparation. `Stale` and `Computing` hashes cannot be announced as valid. Native committed files expose `Verified` hashes while available; directories have `Missing` hash state. Entry status integer values are explicit and covered by catalog regression fixtures. Expose bounded enumeration of opened namespaces/roots. Creation refuses existing unmanaged contents; opening does not import them. Native storage locations are backend configuration, never required for content access.

Logical lookup uses a bounded root-relative sequence of validated name components with `/` separators; reject absolute paths, empty intermediate components, `.` and `..`. The empty path identifies the root. Never pass a logical path directly to an OS path API.

### 5.0 Prepared reads and directory subscriptions

`PreparedRead` owns an already opened reader and reports its `FileInfo` (including expected SHA-256, size, content generation and `HashState::Verified`) for the **same selected source object**. It also reports `ReadConsistency`: `DetectChanges` uses opened-object identity and backend-defined metadata/content validation; `Snapshot` reads an isolated backend-controlled version. Hashing or configured cached-digest validation completes before return, allowing announcement before payload transmission. Preparation fails if reader/metadata identity cannot be bound. `DetectChanges` deliberately does not guarantee immutable source bytes or detection of writes that preserve metadata; Cached/Rehash read-only modes use this guarantee. Native committed generations are pinned against backend replacement/deletion and rehashed during preparation, but remain subject to the native external-change limitations. FileShare additionally verifies actual outgoing bytes before successful completion, and the receiver verifies the complete file before publication. Existing `openRead()` remains unchanged. `ReadPreparationOptions` expresses common requirements such as content-version/expected-hash binding and cancellation, not backend-specific storage paths or policies. Any backend-specific verification mode is configured on its concrete type. The native backend may support this generic operation with its committed, verified file generation. Optional snapshot leases/retention are implementation details but may be referenced by an opaque resume token where supported; a resume never silently switches content versions.

`FileInfo::namespace_id` identifies the backing namespace, including directory entries returned through cross-mount aliases. Route subsequent ID operations using that namespace rather than assuming the listing's original namespace.

Contextual overloads of metadata, listing, reading/preparation and directory subscriptions carry an owned `AccessContext`. Its opaque requester scope, captured/current authorization generation and ID-based check stay outside shared metadata caches. Alias-aware backends additionally use the bounded target resolver and namespace/backend routing callback. `filesystem_check` projects originating exclusions through delegated operations; both provided filesystem backends check resolved objects and traversed ancestors. Other backends fail closed through the optional filesystem guard rather than ignoring that predicate. Filtered cursors are opaque, finite, expiring and bound to the requester scope and policy authority. A retained reader rechecks authorization before returning bytes. There is no mutable global current requester.

`ReadPreparationOptions::resume_lease` and `PreparedRead::resume_lease` carry opaque backend leases where `ResumeLeases` is supported; resumption requires the original expected SHA-256. The native backend rejects lease requests. `IFileReader::invalidate()` reports a mismatch in bytes read from its opened object and invalidates cached content evidence; its optional default is `UnsupportedCapability`. It does not modify source content.


`DirectorySubscription` is a move-only, RAII handle: no copy operations; move transfers ownership, move-assignment releases the destination's old subscription, and destructor/`reset()` unsubscribe without blocking. A successful subscription is bound to `(NamespaceId, DirectoryId)`, has a generation watermark for subscribe/list race detection, and delivers coalesced change/invalidation/`RescanRequired` callbacks through the subscriber's registered Qt execution context. Cancellation stops **new** callbacks; already executing callbacks own safe state and can finish. Never dispatch into a destroyed QObject/context. Manager shutdown invalidates handles safely; directory removal delivers terminal invalidation. Subscription interest may pin a backend's bounded directory cache; impose finite subscriber/pin budgets and retain paginated listing for directories too large to pin. Persist indexed changes regardless of subscribers. Existing namespace-level notifications remain supported.

### 5.1 Directory mutations, listings, and notifications

- Normal directory pages include only entries whose status is `Available` or `Updating`, subject to host/VFS authorization and exclusions; both files and directories may be listed. `Updating` metadata retains last-known display values and may be browsed/subscribed, but only `Available` files can be opened/prepared. Reads of an authorized `Updating` file return `Busy`. `stat`/reconciliation can still report diagnostic `Missing`/`Inconsistent` metadata to authorized local callers. A retained digest on an `Updating` entry is `Stale`, never a current verification claim.
- Rename/move preserve identity and fail on destination collision; normalized no-op renames/moves succeed without generating a mutation. Atomic uniqueness includes pending reservations. No implicit merge or overwrite.
- Move requires a destination directory in the same namespace. Reject cross-namespace moves, self-parenting, descendant cycles, and resulting subtree depth violations. Bound traversal work; do not load an entire subtree into memory.
- Remove files or empty directories only. Return `DirectoryNotEmpty` for directories containing completed or reserved entries. Recursive removal is outside initial scope.
- List with a persisted normalization/case-folded name key and ID tie-breaker using keyset cursors. A cursor binds namespace, directory/filter, and catalog generation; any relevant mutation invalidates it with `StaleCursor`, requiring restart. No unbounded snapshots or offset scans. Apply the same cursor discipline to pending/range/reconciliation pages.
- Notifications are emitted only for committed logical changes and consistency-status changes, on the subscriber's registered execution context. Include namespace and generation; coalesce within a finite queue. Overflow emits `RescanRequired` with a generation watermark instead of retaining unlimited individual changes. Shutdown terminates subscriptions safely.

### 5.2 Readers and replacement

Opened readers bind to a committed content generation. Replacement never retargets an existing reader to new bytes. Preserve the previous generation until readers close where supported; otherwise reject publication with `Busy` while readers exist. Advertise the chosen capability; never silently weaken it. New readers after publication see the new generation. Retained versions count against storage limits.

Externally changed or missing backing files cause `ExternalModification`/`MissingEntry` when detected; mark the entry inconsistent and refuse new normal readers until reconciled. Existing readers fail on detected invalidation rather than claim an intact snapshot. Ordinary size/timestamp/file-identity checks cannot detect every external modification; document that read results are not a guarantee against undetected concurrent external writes.

### 5.3 Typed errors

At minimum: `MissingEntry`, `Conflict`, `InvalidName`, `InvalidArgument`, `PermissionDenied`, `StorageUnavailable`, `UnsupportedCapability`, `Corruption`, `IntegrityError`, `IncompleteContent`, `Cancelled`, `IoError`, `DiskFull`, `QuotaExceeded`, `QueueFull`, `Busy`, `NamespaceBusy`, `DirectoryNotEmpty`, `CrossNamespaceMove`, `ExternalModification`, `StaleCursor`, `IndeterminateOperation`, and `ShuttingDown`. Carry safe structured context and native error codes without requiring sensitive diagnostic strings. A known publication with a durability failure uses the explicit outcome in section 6, not a generic cancellation/I/O result implying no change.

## 6. Native storage, publication, and recovery

Implement with Qt filesystem/SQL support and small platform-specific helpers where necessary. Use a schema-versioned standalone SQLite catalog with fixed regression fixtures. Persist namespace/root IDs, configuration, entries/generations, pending writes/ranges, reservations/accounting, and operation records independently of DarkSpeak.

```text
OriginalName: namespace-root/.filemanager/{catalog.db, staging/, recovery/}
              namespace-root/<logical directories>/<logical filenames>
FileId:       namespace-root/.filemanager/{catalog.db, staging/, recovery/}
              namespace-root/objects/<UUID prefix>/<opaque UUID>
```

SQLite side files and the process lock are internal too. Physical placement of retained generations is private backend state. Catalog names are plaintext initially: opaque physical filenames provide no metadata confidentiality. Neither naming policy implies encryption or secure deletion.

### 6.1 Confinement and permissions

- Anchor operations to validated directory handles; verify filesystem identities and reject symlink/reparse-point traversal, managed hard-link aliases, special files, and nested mount/filesystem transitions. Do not rely on check-then-open path strings. Fail `UnsupportedCapability` when required confinement cannot be guaranteed.
- Reject managed symlinks even if their current target is inside the root. Reserve `.filemanager` and `objects`, including normalized/case-folded aliases, at all logical levels. Never expose staging/recovery/internal entries as user files.
- Place staging on the destination filesystem. No copy fallback that exposes partially published content. Roots must meet required locking, confinement, and publication capabilities.
- Create private directories with owner-only access and files with owner-only read/write, or equivalent platform ACLs. Cover catalog, journal/WAL/SHM, locks, staging, recovery, and managed content in both naming modes. Validate existing permissions on open; reject or explicitly repair unsafe permissions before accepting operations.
- Native-path integration is optional, explicitly authorized, and capability-dependent; the normal interface never requires a physical path. Export/open-in-external-app integration is outside first delivery.

### 6.2 Durable operation protocol

SQLite transactions and filesystem mutations are **not jointly atomic**. Before implementation, define versioned operation records and a fault-injection-tested transition table for create, replace, rename, move, and delete. Minimum phases: `Prepared`, `FilesystemApplied`, `CatalogApplied`, `CleanupPending`, `Complete`, plus explicit rollback/inconsistent states. A phase record can lag the actual filesystem action after a crash; recovery validates identities and evidence rather than trusting phase labels alone.

1. Validate bounds, ownership, reservations, expected object generations, and destination conflicts. Persist an operation ID, intent, recovery identities, and quota reservations before any irreversible action.
2. Prepare/synchronize required data. Apply no-clobber rename for create/move/rename and a supported atomic replacement mechanism for replace. Delete first moves owned content to private recovery storage, then removes the logical entry; reclaim physical bytes after commit and reader release.
3. Record the applied filesystem state, commit the corresponding catalog generation/accounting, and only then release logical visibility and notifications. Keep conflicting API operations serialized across the transition without holding a SQLite transaction across bulk I/O.
4. Persist completion/outcome before forgetting recovery evidence. Cleanup is bounded, restartable, idempotent, and cannot delete files whose ownership/identity no longer matches the operation.

On startup, acquire ownership, recover operations before exposing affected entries, restore safe pending writes, and rebuild/validate quota accounting in bounded batches. Unaffected namespaces remain usable. For each operation, finish or roll back deterministically from durable evidence. Ambiguous state is flagged for explicit reconciliation, never resolved by adopting unrelated content. Unknown staging/recovery objects are quarantined or reported; do not blindly delete them. Keep supported operation-status records for a documented bounded retention window; unresolved outcomes cannot be evicted to admit more work.

### 6.3 Visibility, durability, and cancellation

Atomic visibility means API readers observe a complete old or new logical state. For native create/replace, require atomic filesystem publication as well; OS observers of catalog-plus-filesystem transitions do not receive a cross-system transaction guarantee. Reject unsupported atomic publication rather than silently falling back to partial copies.

Power-loss durability additionally requires file data, SQLite commits, and relevant directory-entry updates to be synchronized in the required order. Document platform/filesystem capabilities and requested durability policy explicitly. Full guarantees depend on filesystem/device behavior; report unsupported durable synchronization rather than claiming it occurred.

`CommitResult`/`MutationResult` contain `OperationId`, final logical generation/state, and an explicit durability outcome. Before irreversible publication, cooperative cancellation may return `Cancelled` with pending state preserved. After filesystem publication succeeds, finish or resolve the transition and return a definite outcome; cancellation cannot turn success into an ambiguous `Cancelled`. A post-publication sync failure reports publication plus durability failure and preserves recoverable operation evidence. If storage failure prevents resolution, return a typed indeterminate-operation result carrying the operation ID, block affected entries, and require `operationStatus()`/recovery before retry; never disguise an uncertain mutation as a safe retryable failure. Process death is resolved through persisted operation and pending IDs.

### 6.4 External changes and reconciliation

Do not automatically import external files or adopt externally modified/replaced content under existing IDs. Detect missing paths, changed filesystem identities, unexpected types/links, and metadata mismatches where possible; flag entries and invalidate cached checksum claims. Metadata checks cannot guarantee detection of all modifications.

Provide explicit bounded scan/report and resolution actions: retry validation, restore known managed state, discard a missing logical entry, or deliberately reimport external content as a new logical file through staging and normal validation. Reconciliation must not silently transfer an existing ID to unrelated content. Never trust a catalog-supplied physical path without confinement validation. Reconciliation obeys quotas, writer reservations, process ownership, and cancellation.

## 7. Portable filename policy

Use one versioned, deterministic managed-native policy for logical names and uniqueness in both physical naming modes. The read-only backend uses separate Linux source-name encoding policy v1 (`encodeLinuxName`): preserve case/normalization and printable Linux filename characters, percent-escape unsafe/raw invalid UTF-8 bytes and literal `%`, and retain original bytes for anchored filesystem resolution. Its encoded logical components allow up to 765 bytes; never URL-decode them into OS paths. The following managed-native creation rules do not reject or rename read-only source entries. Persist the policy/Unicode version; opening with a different implementation must not silently change existing keys. Supply fixed normalization/collision fixtures.

- Bound raw input before processing. Reject ill-formed Unicode, empty names, `.`/`..`, separators `/` and `\`, NUL, control/format characters, Windows-forbidden characters `< > : " | ? *`, and names ending in a dot or space. Do not silently strip dangerous components.
- Normalize accepted names to Unicode NFC. Enforce at most 255 UTF-8 bytes **and** 255 UTF-16 code units per component, further limited by native filesystem capability. Root-relative path length and component count are bounded independently.
- Reject Windows device basenames, including with extensions and case variants (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, and documented Unicode/platform aliases), plus reserved library names. Publish exhaustive fixtures for the policy.
- Compute a uniqueness key using locale-independent full Unicode case folding followed by NFC. Enforce this key even on case-sensitive storage, and also reject collisions under the native filesystem's equality rules. Do not merge preexisting incompatible names during open/reconciliation.
- `Fail` rejects a collision. `UniqueName` tries the accepted name, then `stem (2).ext`, `stem (3).ext`, etc., up to the configured finite attempt limit. Preserve the final extension where it fits; leading-dot names without another dot have no extension. Truncate the stem only at Unicode scalar boundaries to fit the suffix and component bounds; revalidate every candidate. Return `Conflict` on exhaustion.
- Explicit rename/mkdir reject invalid or overlong names rather than truncating them. For `UniqueName` creates, only generated collision candidates may be truncated. Resolve and reserve candidates atomically against catalog entries, pending reservations, and unexpected physical destinations. Never overwrite an uncatalogued destination.

## 8. Execution, cancellation, and lifetime

All public operations are thread-safe; ordering on a handle follows admission order. A simple serialized metadata executor is acceptable; independent bulk I/O remains concurrent. No disk I/O, catalog access, full-file hashing, or blocking waits run on the UI/caller thread.

Document QCoro execution precisely: `QCoro::Task` starts eagerly, admission begins at the API call, and caller arguments become task-owned before suspension. Capture a supported caller execution context; resume continuations there after worker completion, never accidentally on an I/O worker. For callers without such a context, require an explicit dispatcher or reject admission. Public shutdown and subscription APIs use the same rule.

Own shared operation state independently of coroutine frames, manager facades, and handle facades; worker completions cannot access destroyed objects. Dropping a QCoro task does not cancel an admitted operation; callers request cooperative cancellation explicitly through its token. Abandonment does not erase an operation or its recovery outcome. Serialize cleanup after in-flight operations. Destructors never block for disk I/O; normal callers must await shutdown before destroying execution contexts.

Use finite worker, queue, byte, and namespace budgets with fair scheduling across namespaces and handles. Reserve resources before copying/enqueueing large requests; reject with `QueueFull` or provide cancellable bounded admission, never grow an unbounded waiter list. Chunk large reads, writes, hashing, scans, and cleanup so cancellation and other namespaces make progress. No global locks across bulk I/O; keep transactions short and define consistent lock ordering.

Cancellation is cooperative at safe boundaries; OS calls may not be interruptible. Define cancellation parameters/tokens consistently for all tasks. Cancelled writes report actual byte progress and leave uncheckpointed bytes excluded from durable coverage. Async `shutdown()` rejects new work, terminates subscriptions, checkpoints/suspends writers where possible, resolves publication transitions, drains owned operations, closes catalog connections on their owning executor, and releases locks last. Report checkpoint/cleanup failures; never publish merely to simplify shutdown.

## 9. Diagnostics and future application integration

Use structured operational diagnostics with filenames, paths, IDs, and sensitive metadata redacted by default at every log level. Never log file contents. Explicit diagnostic opt-in must be independently controlled by the host; normal errors and notifications do not implicitly log their payloads. Library logging must not bypass DarkSpeak's filename-event preference. Deletion means logical removal and ordinary reclamation; make no secure-erasure promise, including for SQLite history, backups, snapshots, or storage devices.

Future DarkSpeak integration uses a separate VFS composition layer to mount namespaces into per-identity views; the FileManager remains identity-agnostic. DarkSpeak owns identity-to-namespace mapping, incoming-file authorization, mandatory incoming SHA-256, transfer-level verified-range decisions, and user policy. Network transfers use async handles without physical paths; UI models consume logical pages and notifications. Native import/export, previews, drag-and-drop, and existing file-storage migration are separate integration work. Evaluate MCP exposure during that work; this library specification adds no MCP tools or permissions.

## 9.1 Backend extension policy

The separate read-only backend (`read_only_file_manager.md`) indexes existing directories without modifying them; source changes are normal there, unlike the managed native backend's external-change policy. Both satisfy `IFileManager`. `SymlinkPolicy::Always` is a deliberately insecure read-only-backend setting, not a weakening of the native backend's confinement contract. The VFS composition layer (`vfs.md`) handles identity-specific mounts and permissions; no new abstract storage interface is introduced.

## 10. Tests and delivery criteria

Deliver the standalone CMake library, native backend, public headers, documented execution/capability contracts, and an example covering namespace creation/reopen, pending creation/checkpoint/suspend/reopen, streaming/commit, listing, and reading. No DarkSpeak application migration or additional storage backend implementation is included.

Tests must cover:

- Both naming policies, stable identities, persisted configuration, schema/operation/range version fixtures, root overlap aliases, live/stale process locks, and restart ownership.
- Common entry/hash/readiness/capability metadata, `Updating` visibility/read refusal, Linux encoding byte-preservation/collision/expansion fixtures, Unicode/case/platform-name fixtures, length boundaries, collision exhaustion, reserved names, traversal, symlink/hard-link/special-file attacks, mount boundaries, and deterministic race injection.
- Create versus replacement identity, reader generation retention or explicit busy behavior, ancestor writer conflicts, cycles/depth bounds, nonempty-directory rejection, and cross-namespace rejection.
- Sequential/random/overlapping/short writes, EOF, empty files, sparse holes, final sizes, checked arithmetic, range fragmentation limits, and quota accounting/reservations.
- Durable range invalidation/checkpoint ordering, suspension/reopen, crash recovery, full-file hashing after resume, invalidation of incremental hashes, expected-size/hash mismatch, repair and abort. Never accept persisted checksum state as verification after reopen.
- Fault injection and process termination before/after every filesystem/catalog/sync boundary for create, replace, rename, move, delete, and abort; idempotent repeated recovery; foreign-object preservation; disk-full, permission, sync, and cleanup failures.
- Cancellation before/during verification and around publication; explicit post-publication/durability/indeterminate outcomes, abandoned awaiters, handle destruction, and asynchronous shutdown.
- External modification/removal/replacement, explicit reconciliation without automatic adoption, owner-only permissions including SQLite side files, and diagnostic redaction.
- Stable pagination and stale cursors, bounded notifications/rescan fallback, move-only RAII subscription teardown/move/generation races, prepared-read metadata/content binding and hash-before-streaming, concurrent operations, queue/byte limits, fair namespace scheduling, large files without whole-file buffering, and UI-thread responsiveness under sustained I/O.
