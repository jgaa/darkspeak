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
- `prepareRead()` returns `PreparedRead{info, reader}` after hashing the pinned committed generation with SHA-256 in bounded, cancellable worker turns. `info` binds size, digest and generation to that reader and reports `hash_state=Verified`. `consistency=DetectChanges` documents that external writes are detected through the existing evidence checks rather than an immutable snapshot. Optional `expected_generation` and `expected_sha256` reject a different version with `Conflict` and `IntegrityError`, respectively. Invalid hash lengths return `InvalidArgument`. Preparation detects external changes and fails rather than silently adopting content. A prepared reader uses the same replacement/deletion pin and explicit close contract as `openRead()`. No physical paths or snapshot policies enter the common options.


On reopen, missing/replaced staging is flagged. Unknown staging objects are retained, not automatically removed; namespace info reports a minimum untracked count and whether its bounded scan was truncated. Explicit administrator cleanup is required for these unowned objects. Normal reconciliation scans catalog entries; unmanaged external files must be deliberately reimported through ordinary create/write/commit under a new ID.

## Execution and ownership

`QCoro::Task` is eager: admission begins when the API is called, not on first await. Calls require a running Qt event-dispatch context on the calling thread. Continuations return to that context. Arguments and worker state are owned across suspension. Dropping a QCoro task does not cancel it; use `CancellationToken` explicitly and await shutdown before destroying caller execution contexts. A missing dispatcher returns `InvalidArgument`.

Four bounded worker threads execute jobs. Metadata uses a per-namespace mutex with short SQLite transactions; independent readers/writers release it during bulk I/O. Per-handle scheduler lanes serialize operations in admission order. Hashing yields its worker turn between chunks; queues rotate across eligible lanes. Main-thread work is admission/continuation dispatch, not filesystem/catalog/hash work. Ordinary admission is globally capped at 256 jobs and 64 MiB of charged request buffers, further restricted by namespace settings. Up to four bounded hash/read scratch buffers are additionally live. Cleanup has separately reserved bounded admission based on the hard namespace/handle maxima.

Cancellation is cooperative before safe publication boundaries; an OS call need not be interruptible. A short write returns its actual progress. Once publication intent is admitted, its outcome is resolved independently of later cancellation. Abandoned tasks retain state until completion; destroying a facade cannot free buffers underneath a worker.

Await `shutdown()` to reject new work, drain/cancel queued work, checkpoint live writers, close readers/writers/catalogs, and release ownership. Queued work not yet started may return `ShuttingDown`. Checkpoint/recovery failures are reported; shutdown never publishes pending content. Destructors schedule cleanup without waiting for disk I/O; destroying the event loop first violates the caller lifetime contract.

## Quotas and metadata

`NamespaceLimits` defines finite payload, staging, file-size, pending/active-writer/handle, entry, depth, range, page, operation, buffer, collision, notification, and scan limits. Defaults are in the public header and validated against implementation ceilings. Namespace count is capped at 32. Limits persist; `limits_override` explicitly replaces them on reopen. Lowering usage limits blocks further growth without deleting existing data.

Payload accounting uses reserved logical bytes: completed sizes plus pending reservations, including old/new replacement overlap. Sparse holes count toward reservation. Known sizes reserve before writing; unknown sizes grow reservations before extending. Catalog size has its own page-count cap; budget up to twice this cap for catalog plus rollback journal, in addition to payload quota and native allocation/directory overhead. Quotas cannot reserve space against external disk consumers. Cleanup and aborted records can proceed without admitting new payloads.

`EntryStatus::Updating` retains last-known metadata for browsing but refuses content reads/preparation with `Busy`. Ordinary listings contain only `Available`/`Updating` entries; missing/inconsistent entries remain accessible through diagnostic stat/reconciliation. `FileInfo::hash_state` distinguishes `Missing`, `Stale`, `Computing` and `Verified`; an updating file's retained digest is stale. Native namespaces report `NamespaceStatus::Ready` after recovery and advertise `Read`, `Write`, `PreparedRead` and `DirectorySubscriptions`; snapshots and retained resume leases are unsupported.

Listings use normalized/folded-name keys plus UUID tie-breakers and bounded keyset pages. Cursors bind scope and generation; changes produce `StaleCursor`, not an inconsistent continuation. Notifications coalesce into bounded `RescanRequired` generation watermarks on the subscriber's context; consumers reload logical pages. `subscribeDirectory(ns, directory, {context, callback})` validates the directory and atomically registers interest with a namespace generation watermark. Register from the context's Qt thread. The returned move-only `DirectorySubscription` owns that interest: destruction/`reset()` cancels delivery without waiting for I/O, moves transfer ownership, and move assignment first cancels the destination's old interest. Its `generation()` is the registration watermark, not a changing last-delivered value. Native directory subscriptions conservatively rescan on any namespace mutation; they do not pin or load directory contents. Directory removal produces one terminal `Invalidated` notice. Dead contexts, released handles and manager shutdown suppress queued callbacks; already executing callbacks may finish. Handles may safely outlive the manager. Directory and namespace subscriptions share the finite namespace/global notification budgets; abandoned contexts and terminal subscriptions release their slots on subsequent admission. Subscribe before listing, compare `namespaceInfo().generation` against the registration/notification watermarks, and reload on changes or `StaleCursor`. Listing remains paginated regardless of subscription interest. Persisted catalog changes do not depend on subscribers. Library diagnostics exclude file content, entry filenames, IDs and digests. Debug and higher-severity logs exclude paths. Explicit trace logging includes storage root paths and permission details (mode and owner IDs) to diagnose initialization failures; permission failures identify the affected entry role without exposing its filename.

The standalone `encodeLinuxName(QByteArrayView)` utility implements Linux source-name encoding v1 for read-only indexing. It preserves printable UTF-8, case and normalization and percent-escapes control/format characters, Unicode line/paragraph separators/noncharacters, invalid UTF-8 bytes and literal `%`. Source components are bounded to 255 bytes, encoded names to 765 bytes. `.`/`..`, NUL and `/` are rejected. Retain the original bytes for filesystem resolution; encoded labels are never URL-decoded into physical paths. This policy permits Linux punctuation/reserved-looking names and does not replace managed-native creation rules.

Managed-native name policy v1 uses Qt NFC (the Qt Unicode version is persisted) and a checked-in Unicode 17.0 full-fold table. Unicode/policy changes require explicit migration rather than silently changing keys. Components are bounded to 255 UTF-8 bytes and 255 UTF-16 units; controls, format characters, traversal, platform-reserved names, separators, and unsafe suffixes are rejected. Numeric collision suffixes have a finite attempt budget and truncate only at scalar boundaries. Unicode data licensing is in `LICENSE.unicode.txt`.

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

Tests exercise common status/hash/capability metadata, updating-entry visibility and read refusal, deterministic Linux-name encoding and byte-preserving escape fixtures, prepared-read hash/metadata binding, expected-version rejection, timestamp-preserving content tampering, move-only directory subscription ownership, generation watermarks, terminal invalidation, shared budgets, cross-thread delivery and queued cancellation, both naming policies, reopen/resume, coverage/verification invalidation, checksum repair and resumed-file tampering, replacement identity/readers, namespace/process exclusion, traversal/link rejection, directory conflicts, pagination, quotas, large sparse offsets, short writes, disk/sync failures, cancellation, notifications, permissions, schema fixtures, and asynchronous shutdown. Child processes terminate at durable operation/checkpoint boundaries and reopen twice to verify idempotent recovery. These simulate process death; they do not emulate every device's power-loss behavior.

MCP review: standalone storage infrastructure adds no MCP API or permissions. Future DarkSpeak integration needs its own exposure review.

Permission repair is a local runtime option, `NamespaceConfig::repair_permissions` (default `false` for standalone users). When enabled, descriptor-based checks restore owner-only modes (`0700` for directories, `0600` for files) on owned entries as they are accessed, including namespace roots, internal directories, locks and catalog files. Foreign ownership, symbolic links and multiply-linked regular files remain invalid. No ownership changes or recursive filesystem traversal occur. The policy is scoped to each synchronous namespace worker job, including SQLite callbacks; it is not persisted in the catalog.

DarkSpeak enables this option by default through `storage/repairPermissions`. The Files / Paths settings page can disable automatic repairs; a restart applies the change to all current and historical managed namespaces. Disabling repair retains strict permission validation. The option applies only to managed storage, not the whole profile, exports, or arbitrary source files.

The common `IFileManager` is the sole abstract storage interface. Optional `prepareRead()` and `subscribeDirectory()` default to `UnsupportedCapability` for backends that do not implement them. Backend administration belongs on concrete types; ordinary consumers use the common interface. The concrete read-only backend below shares these contracts. Identity-specific mount and permission models remain host concerns and do not change native confinement or external-change policy.


## External read-only sources

Include `<dsfilemanager/readonlyfilemanager.h>`. Construct one application-wide
`ReadOnlyFileManager(private_root)` with an owner-only private directory outside
source trees. `createSource(ReadOnlyNamespaceConfig)` creates an independent UUID
namespace/catalog; `openSource` validates its schema, configuration, root birth/
filesystem identity, and catalog identities. Common `createNamespace` and
`openNamespace` use the default `Never`/`Cached` settings. Source trees are never
created, chmodded, renamed, or written. Mutation and pending-writer operations
return `UnsupportedCapability`.

`NamespaceInfo::status` starts as `Updating`. Bounded worker turns discover raw
Linux directory entries and reconcile them into a private SQLite catalog.
`Ready` means discovery completed, without eagerly hashing files. Individually
validated files can become readable during discovery. Partial scans retain
unseen entries as `Updating`; only stable, successful EOF establishes absence.
Errors remain available in `readiness_error`; retries, scan queues, entries,
catalog bytes, handles, pins, cursors and staging have finite limits. Periodic
reconciliation and root watcher hints run without UI-thread filesystem work.
Scanning uses idle I/O priority where Linux permits it. Directory moves update
one topology node; unambiguous inode/birth-time continuity preserves IDs, while
uncertain matches receive fresh IDs. Inode hints never merge identical copies.

Use the common interface for browsing and reads. Directory subscriptions pin a
bounded directory-metadata LRU; SQLite pages remain bounded independently of
pins. Requester-specific authorization results and pages are never cached there.
Generation changes invalidate cursors. `closeSource` revokes readers and releases
a namespace; manager shutdown also revokes subscriptions and readers.

Read verification is configured on `ReadOnlyNamespaceConfig::verification`:

- `Cached` reuses a digest only with matching source evidence; otherwise hashing
  is lazy and cancellable. `Rehash` hashes before each preparation. Both report
  `DetectChanges`; neither claims the source inode is immutable.
- `Snapshot` copies into separately charged private staging, hashes those bytes,
  and reopens the completed object read-only. It reports `Snapshot`. The opaque
  `PreparedRead::resume_lease` may be passed through
  `ReadPreparationOptions::resume_lease`, together with the original expected
  digest (and optionally generation), to resume the staged version. Resume
  rehashes staged bytes. Expired/revoked/unfinished leases are reclaimed in
  bounded batches, checking recorded file identities before deleting anything.
  A crash between creation and recording an object's identity quarantines it
  rather than guessing ownership. Resume still requires an accessible,
  unambiguously identified source entry for privacy checks.

`charged_bytes` reports reserved snapshots and retained reindex recovery storage; external source bytes are not
backend-owned payload storage. Index space has its own catalog limit. Unsupported
schemas fail explicitly. Offline `reindexSource` preserves the previous private
catalog directory as one `.reindex-backup`; a second rebuild conflicts until that
backup is explicitly dealt with. It never edits the source tree.

### Authorization, aliases and privacy

The host defines `ExclusionPolicy`: existing directory roots and individual
objects with a monotonically increasing generation. Unsafe/unresolvable policies
fail closed. All policies include mandatory backend self-exclusions. Exclusions
are checked through filesystem identities and traversed ancestors, including at
read time; lexical path prefixes are insufficient. Multiply linked regular files
are quarantined because their other aliases cannot be established safely.
Mounted subtrees that hide ancestry are also quarantined. Supported source
filesystems are currently local ext4, XFS, Btrfs, tmpfs and overlayfs with Linux
`statx` mount identity support. Special files are rejected using `O_PATH` inspection
before content I/O. `updateExclusions` atomically revokes affected access before
returning; in-progress operations cannot return bytes under an obsolete policy.

`Never` hides symlinks. `WithinVfs` additionally requires request-owned
`AccessContext` fields: an opaque `authorization_scope`, an authorization
`generation` with shared `current_generation`, an ID-based `check`, and a bounded,
memory-only `resolve_target(FilesystemIdentity)` callback. The optional `backend`
callback resolves cross-backend namespace ownership; use weak captures to avoid
ownership cycles. Without a complete current context, uncertain targets are
hidden, including from listings. Contact/identity models stay in the host.
`Always` additionally allows final external regular files when the trusted
assignment context sets `allow_external_files`; `assignment_directory` identifies
its stable directory boundary. Missing assignment evidence fails closed. External
directory aliases still require independently accessible destinations and are
never recursively indexed through the exception. Exclusions, exposed-entry
authorization and type checks remain mandatory. Both administrative controls use
the red file-only warning specified in `agents.md`.

Authorized destinations use their ordinary backend operations. Returned
`FileInfo::namespace_id` identifies the backing namespace for subsequent ID
operations, including children returned through a cross-mount directory alias. Composition
projects the originating exclusions into `AccessContext::filesystem_check` and
checks objects and ancestors in both supplied backends. A custom backend must
implement the optional filesystem guard before accepting such a request;
otherwise it fails closed with `UnsupportedCapability`. File aliases also retain
a reader guard on the exposed source alias, so link/policy changes revoke a
delegated reader. Cached aliases delegate destination preparation. A Rehash/Snapshot source layers
its own verification/staging on ordinary authorized destination reads, retaining
a destination reader guard so neither backend's exclusions can be bypassed.
Opaque cursor handles bind namespace/directory, requester scope, policy authority,
generation and expiry; they never serialize hidden names or IDs.

DarkSpeak's `ManagedStorage::openReadOnlySource` supplies mandatory application,
credential, native-catalog/lock-registry and export exclusions. It does not grant contacts
access or expose a new network/MCP operation. Ordinary local callers obtain the
shared `IFileManager` through `readOnlyManager`; exclusion administration remains
on the host adapter. MCP impact was reviewed: existing managed-file tools retain
their approved scope; no new MCP tools or wire messages were added.

### Payload verification and tests

`TransferVerifier` hashes offset-ordered payload chunks on bounded workers and
requires exact expected size/SHA-256 before successful completion. Failure is
terminal. The sender uses prepared reads, rebuilds resumed prefixes from the same
reader, excludes retransmissions from incremental hashing, and verifies before
sending a final chunk. A mismatch invalidates the opened version's cached hash
through `IFileReader::invalidate` and sends a source-change failure. Receiver
whole-file verification remains mandatory.

`readonly_filemanager` exercises source names, discovery/restart/moves, all read
modes, lease quotas/expiry, policy revocation, alias isolation/composition, special
files, replacement races, cursor isolation, pinned-cache eviction and paging with
UI heartbeats. Bind-mount checks run in a private user/mount namespace when the
kernel permits it. The managed-storage tests cover mandatory host exclusions and
sender success/resume/timestamp-preserving tampering. `readonly_million_entry_paging`
is separately registered with a 30-minute deadline: enable it with
`DSFM_RUN_MILLION_ENTRY_TEST=1` and at least 6 GiB of free temporary storage. It uses
one million actual source files and explicit finite entry/catalog budgets; normal
runs report that resource-intensive scenario as skipped.

Application VFS composition lives in `../vfs` and delegates through `IFileManager`.
Its directory-alias integration uses `AccessContext::target_context` to select an
explicit destination mount and `descendantAccess` to retain mandatory source
exclusions while serving that alias through another backend. The read-only
backend projects its revocation epoch as well as its identity exclusions.

Composition can request `PageRequest::stable_order` (and `ascii_order`) for bounded
live keyset enumeration. Backends explicitly opt in through
`supportsStableEnumeration()`; the common default rejects unsupported stable
requests. Normal pagination retains generation-consistent behavior. Composition
wraps backend cursors; they never become peer-visible revisions or tokens.
`discardDirectoryCursor` releases abandoned enumeration state idempotently.
