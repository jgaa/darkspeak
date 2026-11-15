# DarkSpeak FileShare Architecture

## 1. Purpose and boundaries

FileShare lets DarkSpeak identities expose selected virtual directory trees for browsing, metadata/thumbnails, download, and eventually controlled upload/exchange. The architecture prioritizes hostile-peer safety, identity isolation, privacy, responsive UI, and low CPU/I/O on large collections. The files below define the implementation contracts; this document defines their relationships rather than repeating their details:

- `file_manager.md`: sole abstract storage interface (`IFileManager`), managed native storage, async file operations, prepared reads, subscriptions, integrity and recovery.
- `read_only_file_manager.md`: application-wide indexing of externally managed source trees, read verification, change reconciliation, safe symlinks and exclusions.
- `vfs.md`: storage registration and identity/group/contact mount overlays producing per-contact effective virtual trees.
- Future specs: groups; authorization/exchange; FileShare server/protocol; browser; transfer queues/history; tags/ratings; SHA-256 content dispositions.

```text
Application-wide read-only sources --+--> IFileManager namespaces --+
                                     |                             |
Identity-owned writable storage -----+                             |
                                                                   v
                                                 Storage registry / identity assignments
                                                                   |
                                      identity mounts + group mounts + contact mounts
                                                                   |
                                               effective VFS per requesting contact
                                                                   |
                                             mount-aware authorization and policies
                                                                   |
                                       FileShare server / existing transfer engine

Groups ------------------------------> mount overlays + authorization
Transfer queues/history -------------> existing transfer engine + authorized VFS refs
Tags/ratings/content registry -------> local metadata/query consumers
```

FileManager implementations know neither DarkSpeak identities nor contacts, protocol, UI, or group policy. VFS composition knows identity and mount topology, but delegates physical storage and indexing. Authorization owns user/group/path policy, credit/quota decisions, and effective-permission explanations; the server enforces those decisions for every remote operation. The browser is a consumer, not another storage implementation. No second FileShare file-transfer engine.

## 2. Single storage interface, registry and ownership

All storage backends implement `IFileManager`; common VFS/server/browser code uses only that interface, capability reporting and typed errors. Add generally useful operations to `IFileManager` with default `UnsupportedCapability`; use casts only for backend-specific configuration/diagnostics. Storage technology is opaque to VFS: a future encrypted RocksDB-backed implementation can replace native managed storage without changing mount or authorization semantics.

The application-wide `ReadOnlyFileManager` indexes externally managed directories without modifying them; private catalogs, scans, hashes and caches are shared across identities. Each identity separately owns its read/write `IFileManager` namespace(s), initially `NativeFileManager`, potentially backed by other implementations later. Identity-owned writable storage is never implicitly available to other identities. Namespace ownership and identity assignments are persistent, explicit and enforced by the storage registry rather than by UI conventions.

An identity may use its own writable namespaces and explicitly assigned application-wide read-only resources (optionally restricted to assigned subtrees). These **identity storage assignments** are hard upper bounds: no group/contact mount or permission rule can expose an unassigned source, escape an assigned subtree, or use another identity's writable namespace. Backend private-data exclusions and supported capabilities are additional non-overridable limits. Assignments do not themselves create remote-visible mount paths.

Stable storage `NamespaceId`/entry IDs are distinct from storage-resource/assignment IDs, VFS mount/node IDs and SHA-256 content identity. Backend directory topology uses parent references: recognized moves modify the directory's parent/name, not descendants; uncertain continuity must not carry forward authority.

## 3. Per-contact VFS composition and permission precedence

The administrator configures storage resources and identity assignments first, then virtual mount definitions at **identity-default, group and contact** levels. Each mount has a stable opaque ID, virtual placement, identity-assigned source namespace/subtree, and independently configured access rules. Permission-only rules can refine existing paths without creating mounts. For each requesting contact, VFS composition overlays identity defaults, all applicable groups and contact-specific definitions into a distinct **effective VFS**. This view is disposable derived state; underlying indexes and physical content remain shared where ownership permits. Equivalent authorized effective views may later share cached derived data.

Mount composition and permissions are separate. A more specific higher-level mount can replace an inherited mount at the same virtual path; independently mounted descendants can appear beneath a parent whose source `RECURSE` is denied. Ordinary source child directories require `RECURSE` to be allowed; explicit child mounts have their own access rules. Different groups may contribute complementary mounts; incompatible group mounts at the same virtual path must be reported as an unresolved configuration conflict, not resolved by iteration order or by exposing both sources. Rules for replacing a parent mount must not accidentally expose inherited descendants; a higher-tier parent replacement shadows inherited descendants, while mounts at the replacing tier can reintroduce them. Permission-only rules do not change topology. See `vfs.md` for the resolver contract.

Permissions are **per operation** and tri-state: `UNSPECIFIED`, `ALLOW`, `DENY`. Resolve each operation from the most specific applicable contact rule, then merged applicable group rules, then identity defaults (which must give definitive values). Within the group tier, `DENY` wins over `ALLOW`, which wins over `UNSPECIFIED`; group grants otherwise combine favorably. A more-specific path rule at a level overrides a less-specific rule for the same permission; `UNSPECIFIED` continues inheritance. A separate path-level **`DENIED`** blocks a whole subtree, including nested mounts, unless a higher-precedence level explicitly reopens it. `DENIED` differs from `RECURSE = DENY`, which only suppresses implicit traversal into source subdirectories. Higher-level explicit allowances can reopen child paths through navigation-only ancestors without exposing those ancestors' source contents. Higher-level explicit allowances can override lower-level denials, but **never** storage assignments, backend exclusions or physical capabilities. The authorization specification will formalize the complete algorithm and exchange accounting; these precedence semantics are architectural requirements.

One source object may appear through several mounts with different permissions. A resolved VFS reference retains `(local identity, requester/contact, effective mount ID, source namespace ID, source entry ID, traversal context)`; every access decision is bound to that **particular mount**, requested operation and current policy generation. Authorization through one mount cannot authorize another. Mount relocation retains ID; delete/recreate yields a new ID. Resolve virtual node IDs with a persisted, versioned opaque derivation from identity/mount/backend entry IDs; persist UUIDs for synthetic directories. Replacing a backend entry with a new ID also changes its VFS identity without per-file VFS persistence.

A subtree mount never exposes source ancestors or siblings via guessed IDs, moves, aliases or subscriptions; cross-mount links require an independently accessible destination except under explicit insecure `Always` symlink policy. Synthetic mounts and backend children have defined deterministic ordering. Name collisions, including those discovered after external changes, must not silently expose an unintended entry. Moving/removing directories containing configured mounts is rejected unless mount configuration is explicitly updated; direct cross-mount move is unsupported (copy-and-delete is an explicit transfer workflow).

Local administration uses an explicit privileged context, and **View as Contact** uses the same effective-VFS and authorization logic as real server requests, exposing the provenance of mounts and effective permissions. Prepared reads, subscriptions, pending writes and other delegated operations retain mount/request context and reauthorize at relevant boundaries (including subsequent reads and commit). Revocation or mount removal invalidates affected handles/leases. No request-scoped authorization decision is cached globally in the application-wide backend.

## 4. Privacy and aliases

A remote contact receives only authorized entries, metadata, timestamps, counts, and notifications. Backend catalog generations include hidden changes and **must not be passed through** as contact-visible revisions. VFS-facing pages/continuations use opaque, authorization-bound cursors and visible-view revisions; hidden changes must not increment exposed revisions or change visible aggregates. This is a metadata-disclosure contract, not a guarantee against every timing side channel. Bound mounts, nesting, subscriptions, cursors, backend pages inspected per request and all retained state; filtered pagination may return an empty page **with a continuation cursor** when its work budget is exhausted.

`SymlinkPolicy::Never` is default. `WithinVfs` resolves a target to a specific independently accessible VFS mount and checks both the link entry and target for the requested operation, including subtree confinement. `Always` additionally permits final regular-file targets outside the assigned VFS when both the backend setting and the identity storage assignment explicitly enable this administrative capability. External directory links still require independent `WithinVfs` access. Intermediate directory links may resolve an allowed file but are never exposed by the exception. Backend exclusions and special-file restrictions remain absolute. Both settings require a red warning describing individual external files, not external directory traversal; see `read_only_file_manager.md` for resolution details.

## 5. Indexing, reads and subscriptions

The read-only backend opens its private persistent index and performs bounded, low-priority lazy discovery/reconciliation. Qt filesystem watchers are hints; use size/mtime initially, inode/filesystem identity and optional directory-entry fingerprints as move hints, and never infer absence from an incomplete scan. Changed/unvalidated entries remain visible as `Updating` with previous metadata where authorized, but cannot be prepared for download until validated. Hashes are calculated on demand, with SHA-256 available **before payload transmission**.

Per backend instance: `Cached` reuses hashes based on validated metadata; `Rehash` recalculates before transfer; `Snapshot` stages a private quota-limited copy, hashes it, and streams from that same isolated copy. For Cached/Rehash, source-change detection is best effort, and the transfer layer verifies the actual bytes it sent; the receiver always verifies the expected full SHA-256 before publication. Snapshot leases and resumed transfers remain bound to the original expected content. The FileShare server obtains expected hash and reader together through `prepareRead`, not separate path-based lookups.

The common move-only RAII `DirectorySubscription` unsubscribes on destruction/reset; models and sessions retain subscriptions while using directories. Implementations may pin bounded per-directory LRU entries while observed. Coalesce targeted notifications for observed directories and provide generation/watermark plus `RescanRequired` on overflow. Persistent indexing proceeds independently of subscriptions. Qt worker work, hashing and SQLite access never block the UI thread.

## 6. Server, export and browser

The FileShare server provides bounded listings, metadata, thumbnails and downloads through the authorized VFS; uploads later use managed writable FileManager namespaces. Thumbnail parsing/generation is bounded and isolated from the UI, with a shared cache where safe. A local Dolphin-like browser uses the same VFS model as remote browsing; remote catalogs support fast offline navigation, view/sort/filter controls and optional thumbnails. OS drag/drop uses existing transfer/materialization for remote objects and managed imports for writable destinations.

Initial complete-catalog export: generate an **authorization-filtered, size-limited staged artifact** with per-directory consistency and a versioned format. Hash the completed artifact and transfer it as an immutable file using the existing file-transfer mechanism. The catalog is historical metadata: a later file download is separately authorized and verified. Export failure/limit exhaustion returns a bounded error; it must not silently produce a misleading purportedly complete catalog.

Future optimization, not required initially: canonicalize the effective *visible result* (mounts/subtrees, permission-dependent visibility, selected fields, format version, identity-specific metadata if included) into a cache key independent of the requesting contact. Authorized contacts with the same effective result may reuse an artifact; knowing its key confers no access. Backend changes can patch entries to `Updating` in a cached representation, generate new immutable uniquely named artifacts, and retain old versions under bounded leases while readers finish. Hidden changes cannot alter exposed view revisions. Streaming an unfinished export would require a separately defined integrity/framing contract because ordinary transfers announce final SHA-256 first.

## 7. Other modules and implementation sequence

Groups are reusable identity-scoped contact collections, not a storage-backend feature. Authorization/exchange implements the precedence and path semantics fixed in §3, effective-permission explanations, upload/drop-box rules, ratio credit and quotas; it exposes request-scoped decisions and reauthorization to VFS/server. Transfer authorization is distinct from available exchange credit. The full decision algorithm and accounting remain for that specification.

Persistent transfer queues use the existing transfer machinery; retain peer/identity/mount/context and expected content identity for resume. Completed-download history may be presented as another virtual browser provider without duplicating content. Tags/stars and SHA-256 disposition lists are independent local metadata; content-level metadata and occurrence-level metadata have different scope. A shared query/filter facility may evolve as needed without becoming a new physical storage engine.

Implement and test the FileManager common extensions and read-only backend, then storage registry and per-contact VFS composition with mount-context authorization hooks. Add groups/authorization before exposing remote browsing; then server, browser and transfers; then exchange and metadata features. Cross-module tests focus on cross-mount permission isolation, subtree escape, revocation of active readers, filtered pagination and hidden generations, sharing one backend across identities, staged catalog export, symlink exclusions, and nonblocking behavior under large indexes.
