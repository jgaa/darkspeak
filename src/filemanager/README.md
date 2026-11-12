# DSFileManager

Standalone C++20 virtual filesystem with QCoro tasks, a Qt SQL catalog, persistent pending writes, and a native Linux backend. DarkSpeak application migration is separate; this library does not depend on its protocol, identities, UI, Tor, or secret store. Public types are in `include/dsfilemanager/filemanager.h`, namespace `dsfm`; installed target: `DSFileManager::FileManager`.

## Build and consume

```sh
cmake -S src/filemanager -B /tmp/dsfm-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/dsfm-build -j2
ctest --test-dir /tmp/dsfm-build --output-on-failure
/tmp/dsfm-build/dsfm_example
cmake --install /tmp/dsfm-build --prefix /tmp/dsfm-install
```

Requires Qt Core/SQL (SQLite driver), QCoro Core, system SQLite development files, logfault, and threads. The Qt SQLite plugin must use the same system SQLite instance: an incompatible/bundled plugin cannot open the descriptor-relative VFS and fails closed. CMake finds QCoro/logfault packages first; source fallbacks support `DSFM_QCORO_SOURCE_DIR`, `DSFM_LOGFAULT_SOURCE_DIR`, and standard FetchContent offline overrides. No additional Qt modules are required by the library. Tests use Qt Test. Dependency versions and options are declared in CMake.

```cmake
find_package(DSFileManager CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE DSFileManager::FileManager)
```

Set `DSFM_BUILD_TESTS=OFF` and `DSFM_BUILD_EXAMPLE=OFF` for a library-only build. Test fault injection is compiled into a separate, uninstalled support library; the installed library does not inspect fault-injection environment variables. The example creates temporary storage and demonstrates create, write, suspend, reopen, commit, listing, and reading.

## Native support boundary

The initial backend requires Linux `openat2`, `renameat2`, descriptor-relative operations, process locking, and file/directory synchronization. Other platforms are rejected at configure time pending equivalent native helpers. Supported filesystem types are local ext-family, XFS, Btrfs, tmpfs, and overlayfs; required syscall/filesystem failures are returned explicitly. Network filesystems are rejected because local ancestor-lock coordination cannot enforce ownership across hosts. tmpfs supports process restart recovery but is not persistent across reboot. Hardware/filesystem behavior limits power-loss guarantees; no userspace implementation can override a device that lies about synchronization.

Root paths are absolute and clean, with existing parents. Root and all private storage must be owned by the effective user with owner-only permissions. Symlinks, managed hard links, special files, nested mounts, overlapping managed roots, and reserved logical names are rejected. A shared per-user `/tmp/dsfm-locks-UID` registry locks ancestor filesystem identities; an additional `owner.lock` inside the namespace protects direct ownership. Cooperating processes must share the ancestor-lock registry/mount view. Root/internal-directory identity is rechecked on operations. Arbitrary privileged or same-user filesystem tampering is outside the isolation guarantee; detectable inconsistencies fail closed and external content is never automatically adopted.

OriginalName mirrors logical directories and normalized filenames. FileId uses `objects/<UUID prefix>/<UUID>` with catalog-only logical directories. Both have a private `.filemanager` catalog/staging/recovery area. Changing naming policy on reopen is rejected. Catalogs and physical files are plaintext; opaque names are not confidentiality. Deletion is logical removal and reclamation, not secure erasure.

## Pending writes and integrity

- File, directory, namespace, pending, and operation IDs are locally generated UUIDs. IDs are namespace-scoped; names/paths are not identifiers.
- `createFile()` reserves a new ID/name. `replaceFile()` reserves new content for an existing ID. New content is invisible to normal readers until commit. Replacement leaves old content available beforehand.
- One pending write reserves a file until commit/abort, even while suspended. Conflicting entry/ancestor mutations are rejected. Only one writer handle owns a pending ID.
- `writeAt()` reports actual bytes written. Dirty coverage is not durable coverage. `checkpoint()` synchronizes data before committing coverage. Overwrites durably invalidate old storage-valid/application-verified ranges before touching bytes.
- `markVerified()` checkpoints and records caller verification of fully written ranges. It is not a checksum calculation or peer authentication guarantee. Overwrites invalidate these flags.
- `suspend()` checkpoints and releases the handle. Reopen through `openPending()`; inspect using paginated `listPending()`/`listRanges()`. Destroying a writer releases ownership without publishing or implicitly aborting; only checkpointed coverage is recoverable.
- Unknown-size writes must call `setFinalSize()`. File length/sparse holes do not prove coverage. `commit()` requires exact size and full storage-valid coverage.
- Commit hashes the complete content with SHA-256 in bounded worker chunks, including all resumed/random-access/overwritten data. Incremental hashing is deliberately not implemented; it is an optional future optimization. No persisted checksum state is trusted on reopen. Expected constraints are immutable. DarkSpeak must supply an expected SHA-256 for every incoming transfer during future integration.
- Digest mismatch returns `IntegrityError`, retains the pending write, and never publishes it. Repair the bytes and retry, or abort. Old committed replacement content remains readable.
- `abort()` is journaled and recoverable. `abortPending()` also releases suspended/missing/inconsistent pending writes without deleting an unrelated object substituted at their staging path.
- Open readers pin a committed generation. This backend advertises `retain_readers_on_replace=false`: replacement publication and deletion return `Busy` while readers are open. Readers never silently switch generations.

On reopen, missing/replaced staging is flagged. Unknown staging objects are retained, not automatically removed; namespace info reports a minimum untracked count and whether its bounded scan was truncated. Explicit administrator cleanup is required for these unowned objects. Normal reconciliation scans catalog entries; unmanaged external files must be deliberately reimported through ordinary create/write/commit under a new ID.

## Execution and ownership

`QCoro::Task` is eager: admission begins when the API is called, not on first await. Calls require a running Qt event-dispatch context on the calling thread. Continuations return to that context. Arguments and worker state are owned across suspension. Dropping a QCoro task does not cancel it; use `CancellationToken` explicitly and await shutdown before destroying caller execution contexts. A missing dispatcher returns `InvalidArgument`.

Four bounded worker threads execute jobs. Metadata uses a per-namespace mutex with short SQLite transactions; independent readers/writers release it during bulk I/O. Per-handle scheduler lanes serialize operations in admission order. Hashing yields its worker turn between chunks; queues rotate across eligible lanes. Main-thread work is admission/continuation dispatch, not filesystem/catalog/hash work. Ordinary admission is globally capped at 256 jobs and 64 MiB of charged request buffers, further restricted by namespace settings. Up to four bounded hash/read scratch buffers are additionally live. Cleanup has separately reserved bounded admission based on the hard namespace/handle maxima.

Cancellation is cooperative before safe publication boundaries; an OS call need not be interruptible. A short write returns its actual progress. Once publication intent is admitted, its outcome is resolved independently of later cancellation. Abandoned tasks retain state until completion; destroying a facade cannot free buffers underneath a worker.

Await `shutdown()` to reject new work, drain/cancel queued work, checkpoint live writers, close readers/writers/catalogs, and release ownership. Queued work not yet started may return `ShuttingDown`. Checkpoint/recovery failures are reported; shutdown never publishes pending content. Destructors schedule cleanup without waiting for disk I/O; destroying the event loop first violates the caller lifetime contract.

## Quotas and metadata

`NamespaceLimits` defines finite payload, staging, file-size, pending/active-writer/handle, entry, depth, range, page, operation, buffer, collision, notification, and scan limits. Defaults are in the public header and validated against implementation ceilings. Namespace count is capped at 32. Limits persist; `limits_override` explicitly replaces them on reopen. Lowering usage limits blocks further growth without deleting existing data.

Payload accounting uses reserved logical bytes: completed sizes plus pending reservations, including old/new replacement overlap. Sparse holes count toward reservation. Known sizes reserve before writing; unknown sizes grow reservations before extending. Catalog size has its own page-count cap; budget up to twice this cap for catalog plus rollback journal, in addition to payload quota and native allocation/directory overhead. Quotas cannot reserve space against external disk consumers. Cleanup and aborted records can proceed without admitting new payloads.

Listings use normalized/folded-name keys plus UUID tie-breakers and bounded keyset pages. Cursors bind scope and generation; changes produce `StaleCursor`, not an inconsistent continuation. Notifications coalesce into bounded `RescanRequired` generation watermarks on the subscriber's context; consumers reload logical pages. Subscriber contexts must remain alive until unsubscribe and outstanding work have drained; await shutdown before destroying dispatch contexts. No file content or private filename/path/ID/digest is included in library diagnostics.

Name policy v1 uses Qt NFC (the Qt Unicode version is persisted) and a checked-in Unicode 17.0 full-fold table. Unicode/policy changes require explicit migration rather than silently changing keys. Components are bounded to 255 UTF-8 bytes and 255 UTF-16 units; controls, format characters, traversal, platform-reserved names, separators, and unsafe suffixes are rejected. Numeric collision suffixes have a finite attempt budget and truncate only at scalar boundaries. Unicode data licensing is in `LICENSE.unicode.txt`.

## Publication and recovery protocol v1

SQLite transactions and filesystem operations are separate durability domains. Operation payloads persist intent, object identities, source/destination, content constraints/digest, and result generation. Only matching filesystem identities may be moved or reclaimed.

| Operation | Prepared filesystem action | Catalog application | Cleanup |
|---|---|---|---|
| Create | No-clobber staging-to-destination rename | Insert reserved ID and verified metadata; remove pending | Retire operation |
| Replace | Atomic exchange of staging and old destination | Update the same file ID; remove pending | Remove old generation retained at staging path |
| OriginalName mkdir | No-clobber staged-directory rename | Insert directory ID | Retire operation |
| Rename/move | No-clobber rename, or catalog-only for FileId | Update name/parent under same ID | Retire operation |
| Delete | Move to private recovery path | Remove logical entry | Reclaim matching recovery object |
| Abort | Move owned staging to recovery, or catalog-only if absent/substituted | Remove pending/reservation | Reclaim matching recovery object only |

Persisted phases: `Prepared` → `FilesystemApplied` → `CatalogApplied` → `CleanupPending` → `Complete`. A safely detected pre-publication destination conflict becomes `RolledBack`. Records can lag a completed filesystem action; recovery inspects source/destination identities rather than trusting phase labels. Reopening reruns unfinished transitions idempotently and rehashes recovered create/replace candidates before API availability. Ambiguous state returns `IndeterminateOperation` with an operation ID and blocks normal namespace operations; `operationStatus()` remains available for inspection.

Atomic visibility and synchronized durability are distinct. A known publication with a sync failure returns its generation and `Durability::SyncFailed`; the operation remains `DurabilityPending`, retaining evidence for a later synchronization/recovery attempt. Cancellation never disguises publication as a safely retryable cancelled operation. `operationStatus()` reports known generation/durability outcomes. Unresolved records are never evicted; completed/rolled-back history retains approximately the last 1024 operation sequence positions. Physical cleanup is identity-checked and restartable. Repeated recovery cannot adopt foreign content or delete arbitrary files.

A descriptor-relative SQLite VFS keeps catalog and rollback-journal operations anchored to the validated private directory, with owner-only files and explicit file/directory synchronization. It does not support WAL or temporary disk databases. SQLite connection use is serialized by namespace ownership/scheduling, so its VFS lock callbacks need not implement a second independent lock protocol.

## Validation

Tests exercise both naming policies, reopen/resume, coverage/verification invalidation, checksum repair and resumed-file tampering, replacement identity/readers, namespace/process exclusion, traversal/link rejection, directory conflicts, pagination, quotas, large sparse offsets, short writes, disk/sync failures, cancellation, notifications, permissions, schema fixtures, and asynchronous shutdown. Child processes terminate at durable operation/checkpoint boundaries and reopen twice to verify idempotent recovery. These simulate process death; they do not emulate every device's power-loss behavior.

MCP review: standalone storage infrastructure adds no MCP API or permissions. Future DarkSpeak integration needs its own exposure review.
