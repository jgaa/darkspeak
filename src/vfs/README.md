# VFS composition

`dsvfs::Vfs` composes application-wide `dsfm::IFileManager` registrations into
identity/group/contact mount definitions and request-bound effective trees. It owns no physical source index, content hash cache,
copy engine, or transfer protocol. Read-only registrations can be shared by many
identities. Writable registrations require an explicit owning identity matching the persistent resource registry; mount
creation and subsequent use enforce that owner, including after restart.

The application adapter is `ManagedStorage::openVfs()`. It supplies private export
storage and registers opened read-only sources with the application's mandatory
exclusions. Other hosts can construct the library directly, register backends, configure resources and identity assignments,
and supply an application-private managed export namespace with `setExportStore`.
That namespace cannot also be a mounted source. Host shutdown closes the VFS
before its backends. Keep the QObject alive until outstanding public operations
finish; call its methods from its owning Qt thread.

## Resources, assignments and effective topology

Runtime `registerNamespace` does not grant access. Administrators call
`addResource` to persist namespace ownership and `assignStorage` to assign a
stable directory ID to an identity. Multiple writable resources per identity are
supported. `withdrawAssignment` revokes access through its mounts and delegated
handles. Administrative changes serialize against active commits. Backend
exclusions and current source ancestry remain mandatory independently of peer
permission callbacks. Assignment IDs are never silently recycled.

Legacy `addMount` remains an identity-default administration API and selects an
explicit matching non-external assignment unless its `Mount::assignment` is
provided. Tiered configuration uses `defineMount`, `relocateDefinition` and
`removeDefinition`. `effectiveMounts` reports selected definitions and contributing
principals for trusted local provenance/explanations. Source-backed IDs remain stable across relocation. Persisted
synthetic navigation UUIDs give missing/denied ancestors stable identities.

`compose(identity, request)` selects identity defaults, trusted host-supplied
groups, and contact overrides into an immutable effective topology retained by
the returned request. Tier precedence comes before path specificity. Parent
replacement shadows inherited descendants; same-tier descendants can reintroduce
them. Incompatible group sources at one path fail closed and emit coalesced Home
conflict signals. Legacy mounts also participate as identity defaults. A new
configuration generation invalidates previously composed contexts; compose again
using current host authority. Keep this same request for browsing, delegated
operations and catalog transfers. Effective state is disposable; source catalogs
and indexes are shared.

`PermissionPolicy` resolves immutable request-owned rules per operation:
contact, independently resolved/deny-merged groups, then definitive identity
defaults. Path-level subtree denial and virtual navigation differ from source
`Recurse`; explicit child mounts survive a nonrecursive parent. A higher-tier
child grant can create a navigation-only route without listing denied source
contents. `Export` uses listing permission. The existing `authorize` callback is
still required and can impose additional host restrictions; administration uses
`ConfigureMounts` separately. Group membership and policy persistence belong to
the host authorization module. No remote browsing RPC is enabled by these APIs.

## Authorization

Every operation requires a `Request` with an opaque requester `scope`, a captured
policy `generation`, a shared atomic `current_generation`, and a bounded,
thread-safe `authorize(Target, Operation)` callback. There is no implicit local
or remote allow context. Local administration supplies an explicit owner context;
`ConfigureMounts` is separate from file creation/write permission. Permission
rules and contact/group models belong to the host's authorization component.
Map `Export` to listing permission and `Traverse` to ancestor traversal;
`ConfigureMounts` is owner administration, not a contact upload grant.

A target includes identity, mount, namespace, and source object. The VFS checks
source-subtree ancestry and traversal through virtual mount parents. A grant in
one mount does not grant access through a second mount of the same source. Hidden
entries appear absent. Mutations additionally require a `commit_permit` whose
lifetime serializes publication against host policy revocation. The host must
publish permission, group, preference, and relevant topology changes through its
policy generation; it must not withdraw permissions while a corresponding
commit permit is held. New-file writers use `Create`, allowing upload-only
policies; replacement writers require `Write`.

`WithinVfs` aliases require `resolve_target(FilesystemIdentity)` to select one
explicit destination mount, and `within_subtree(Target, root)` to supply current
bounded memory-only evidence. Unknown evidence fails closed. These callbacks
must not perform filesystem/catalog I/O or reenter a backend. Their evidence is
an admission check: the VFS also verifies actual catalog ancestry asynchronously
before returning metadata and around payload reads. The backend binds resolution
to opened filesystem identities. Directory aliases use authenticated route IDs
that retain the exposed path and selected target; independent aliases remain
distinct virtual entries. Source exclusions are projected through
`IFileManager::descendantAccess`; directory delegation's `target_context` keeps
overlapping mounts' authorization contexts distinct. Neither hook is a second
storage abstraction. `Always` additionally requires `StorageAssignment::allow_external_files` for external regular files; external directory links always require an independently authorized destination. Both settings require the red file-only warning in `agents.md`;
`NodeInfo::symlinks` lets UI consumers display the required red warning.

Readers retain authorization, mount, subtree and exclusion checks. Snapshot resume
tokens encrypt their original backend version and bind identity, requester scope
and the exact virtual node; a lease cannot be reused through another mount.
The common reader `accessGuard()` provides a memory-only policy check for queued
offers and retransmissions, while actual reads still validate the filesystem. Policy
revocation, mount removal, conflicts, expiry, and shutdown prevent further reads.
The refresh timer releases idle revoked/expired reader handles and export files.
Only a valid host commit permit can publish pending writes; mount changes cannot
race an active publication. Already delivered bytes/finished commits are not
recalled.

## Identity, persistence, and conflicts

The owner-only private directory has a single-process flock, an atomically
replaced `mounts.json` version 2 registry, and disposable SQLite view data.
Persistence is off the UI thread. Unsupported versions fail closed. The registry
contains persisted synthetic root UUIDs, mount records, and a private derivation
key. Source-backed node encoding v1 uses deterministic keyed nonces and
libsodium authenticated encryption of fixed identity/mount/namespace/source
UUIDs. Authenticated alias routes compose these tokens with a separate MAC
domain. Decoding does not require per-file persistent mappings. IDs remain
stable across restart and ordinary virtual rename/move; mount recreation and
new backend object IDs produce fresh IDs. IDs never grant authority.

Mount updates validate names, parent dependencies, cycles, depth, owner isolation
and collisions before durable publication. Ordinary directory rename/move/remove
cannot invalidate nested mounts. Cross-mount moves are rejected even when source
namespaces match. Backend operations retain their own transaction/error semantics.

A filesystem-created mount-name conflict makes the ambiguous entry inaccessible.
The VFS coalesces trusted local conflict signals per mount; `ManagedStorage`
forwards these to `NotificationsModel`, which persists a Home-pane notification.
Resolving/removing the conflict removes its notification. Conflict details are
never remote catalog records. An unavailable mount is omitted from a composed
parent view; directly addressing that source preserves its unavailable error.

## Browse and subscriptions

Browse enumeration inspects bounded backend batches and merges them with effective
mounts and synthetic navigation entries in UTF-8 byte order of presented names,
then opaque node IDs. An empty page with a continuation is valid. Request-owned
state retains only bounded buffers and the backend keyset position; the first
page never requires sorting a complete directory. Cursors are opaque,
authorization/topology/presentation-bound and finite-lived. The native and
read-only backends explicitly support `PageRequest::stable_order`, including an
indexed ASCII presentation order. Other implementations fail with
`UnsupportedCapability` until they implement that contract. Hidden catalog
generation changes do not restart enumeration. Entries are checked again before
emission; stale historical metadata is marked `Updating`. New entries before the
already emitted key wait for the next enumeration. Completed catalog exports
and subscription digests still use bounded private SQLite snapshots with
per-directory consistency. Early lookup completion releases backend cursors.

Directory revisions hash visible metadata, not backend counters. Directory
physical timestamps/size, hashes, and freshness are not exposed: a physical
directory becoming stale can result solely from hidden child changes. Directory
containers remain `Available`; visible file entries retain backend `Updating`
and cannot be prepared for reading until validated. File timestamps are reduced to UTC
days. `allow_utf8=false` presents logical labels as ASCII percent escapes,
including escaping literal `%`; labels are never decoded into OS paths. Use
opaque IDs for operations. The host sets this flag from the peer's preferences.

Move-only subscriptions pin the underlying directory and alias targets, retain
stable IDs through virtual moves, and combine source hints with mount changes.
Notifications compare filtered view digests, so hidden-only changes do not emit
visible revisions. Destroying/resetting a subscription releases its pins; mount
removal/revocation invalidates it. Filtering/reclamation runs in bounded worker
jobs and yields to the Qt event loop. This protects explicit metadata/protocol
signals; it does not claim to eliminate timing side channels.

## Catalogs and transfer integration

`exportCatalog` stages a versioned JSON-lines catalog through the supplied managed
backend. It supports one directory or bounded depth-first recursion across mounts,
with per-directory consistency rather than a global transaction across catalogs.
The format carries virtual IDs, logical names, status/hash state, capabilities,
privacy-reduced metadata and backend symlink warning policy. It contains no local
paths, namespace IDs, source IDs, hidden counts, or raw backend generations.
Generation never leaves database transactions open waiting for network traffic.
Limits fail explicitly without publishing a partial export.

`CatalogExport` exposes size, SHA-256 and an authorization-checked `prepareRead`.
The backend computes the staged content hash. Readers/resume reproduce the same
committed content; exclusion policy epochs and mount dependencies remain live.
Generation plus retained/transmitting exports share a default global admission
limit of two; excess requests get `QueueFull` (there is no additional unbounded
queue). Exports have finite total/idle lifetimes. They are disposable after
restart: obtain a fresh authorized export rather than reconstructing authority
from an old transfer row.

`FileManager::addVfsTransfer(conversation, vfs, node, request, mime)` prepares a
matching reader/hash before queueing an ordinary VFS file through the same sender.
Its initial reader is retained; retries/resume reprepare that authorized version
and preserve snapshot leases. Catalog labels stay unchanged; `FileManager::transferName` deterministically
escapes/shortens a download suggestion when the existing 255-byte wire/managed
filename policy requires it. The host supplies a locally determined MIME type
(default `application/octet-stream`), subject to recipient policy. Source
bindings are transient and must be freshly authorized after restart; an old file
row with no backing storage/provider cannot emit a new offer. Pending writer
reservation metadata returned for local adapters is not a peer protocol object;
keep backend reservation IDs local.

`FileManager::addCatalogTransfer(conversation, catalog)` binds an export to its
recipient and identity, checks MIME/text preferences, and queues it through the
existing file-transfer engine. For this adapter, `Request::scope` must equal the
contact UUID. The existing eight-chunk byte window, cumulative acknowledgements,
resume, cancellation and payload digest verification remain unchanged. Offering,
reading, retransmitting and completing recheck the binding; a queued or loaded
transfer cannot bypass revocation. Unicode catalogs are rejected for peers that
disable UTF-8; generate with `allow_utf8=false` for those peers.

`validateCatalog` incrementally validates hostile received content with bounded
records, canonical numeric/ID fields, version checks, ordered parent topology,
unique node IDs, entry/depth/byte/deadline limits, cancellation, and whole-payload
SHA-256. A remote-catalog consumer must validate a completed immutable managed
object before atomically replacing its cached view; validation failure leaves
that previous view intact. Receiving a catalog does not grant download access.
The FileShare request protocol, permission-rule implementation, browser UI and
remote cache consumer remain separate application components. Sender cache-file
reuse and composed immutable readers/patches in spec §6.2 are future optimization.

MCP impact reviewed: registry, assignment and overlay administration are candidates
for a future explicitly approved MCP surface; this change exposes none. Existing MCP
contact-request notification queries are restricted to their existing type;
local VFS conflict notifications do not change those schemas.

## Tests

`vfs` is registered with CTest (120-second timeout) and included by
`tests/run-all-tests.sh`. It exercises real managed/read-only backends, ID and
format fixtures, mount aliases, scope isolation, subtree traversal, revocation,
mutations, snapshots of browse metadata, subscriptions, exclusions, conflicts,
exports, resume and hostile format parsing. `managed_storage` additionally tests
Home notification persistence and real outgoing catalog channel completion and
revocation. No public Tor or real profile is used.

For an isolated library-only build:

```sh
cmake -S src/vfs -B /tmp/darkspeak-vfs-tests -G Ninja \
  -DDSFM_BUILD_TESTS=ON -DDSFM_BUILD_EXAMPLE=OFF
cmake --build /tmp/darkspeak-vfs-tests --target vfs_test
ctest --test-dir /tmp/darkspeak-vfs-tests -R '^vfs$' --output-on-failure
```

Use `DSFM_QCORO_SOURCE_DIR` / `DSFM_LOGFAULT_SOURCE_DIR` for offline dependency
checkouts, as with FileManager's standalone tests.
