# DarkSpeak FileShare Protocol — remote read operations v1

**Status:** Initial feature-wire specification. **Server behavior:** `fileshare_server.md`. This document specifies only FileShare-specific control messages and presentation; it does not alter DarkSpeak protocol v3 handshake, generic transfer checkpoints, protobuf framing, or transport cryptography. Actual declarations belong in `proto/darkspeak.proto` when implemented. The sketches below are normative about field semantics and constraints, not reserved numeric tags: select unique protobuf tags against the then-current schema.

## 1. Feature negotiation and trust boundary

- Advertise the case-sensitive capability `fileshare-v1` only when the identity has FileShare enabled, server handlers initialized and current authorization policies available. Both endpoints must advertise it before sending FileShare controls. A peer lacking the capability can still use unrelated messaging and generic file transfers. Do not implement an earlier FileShare compatibility mode.
- Place all feature messages in existing authenticated, encrypted, size-bounded v3 `Envelope` bodies, with the existing monotonic request IDs and `response_to_request_id` correlation rules; never accept peer-supplied local identity selection. Malformed shapes, unknown security-critical enum values, duplicate correlation, cross-connection response IDs, invalid encodings and impossible offsets are rejected before feature dispatch. Apply current protocol error/close policy.
- The trusted local connection supplies `identity`, `contact`, security preferences and connection generation. Requester paths, returned node IDs, listing cursors, transfer IDs and catalog IDs are **not capabilities**. Every operation resolves and reauthorizes current mount, namespace and requested operation.

## 2. Portable UNIX-like virtual paths

Every path-carrying message uses a single typed object:

```proto
enum VirtualPathEncoding {
  VIRTUAL_PATH_ENCODING_UNSPECIFIED = 0; // reject
  VIRTUAL_PATH_UTF8 = 1;                // plain Unicode VFS presentation names
  VIRTUAL_PATH_ASCII_ESCAPED = 2;       // percent-escaped UTF-8 bytes
}

message VirtualPath {
  VirtualPathEncoding encoding = 1;
  string path = 2; // absolute UNIX-style logical path; "/" is root
}
```

- Use a **single leading `/`**, `/` between components, no trailing `/` except root, no repeated `/`, no empty component, `.`, `..` or NUL. Backslash is an ordinary *logical name character*, never a separator. Maximum path components, string bytes and per-component logical length follow fixed documented feature limits (recommended initial hard caps: 64 components, 4096 encoded bytes overall, no less than the existing VFS component bound). Reject excessive input **before** resolving or allocating proportional work.
- **UTF8:** protobuf string must contain well-formed UTF-8; path components are the exact Unicode logical presentation labels issued by the VFS (do not silently normalize, change case, transliterate or sanitize). Literal `%` is an ordinary character. Do not infer an escape encoding from the text.
- **ASCII_ESCAPED:** the `path` contains only ASCII bytes. Each component represents the UTF-8 bytes of its **VFS logical presentation label** (not the source filesystem raw filename). Keep ASCII printable bytes 0x20–0x7E except literal `%` unescaped; represent every other byte and literal `%` as uppercase `%HH` (exactly two uppercase hex digits). Separators `/` are structural and **never percent encoded**. A complete percent triplet must encode only a byte requiring escaping under this rule; reject lowercase/noncanonical or needless encodings. Apply the existing VFS ASCII presentation function (`Request::allow_utf8=false` / `presentName`) for outward encoding rather than inventing a second independent policy.
- After bounded decoding of ASCII_ESCAPED, validate that bytes form the VFS's well-formed UTF-8 logical label. Reject an encoded slash (`%2F`), NUL, forbidden control/format scalar, empty/`.`/`..`, invalid sequence and a decoded logical component violating limits. **Decode only this outer transport/presentation layer once** for a `dsvfs::Vfs::lookup` logical component. The underlying read-only source-name encoding (e.g., `encodeLinuxName` turning an invalid Linux byte into `%FF`) is a distinct, already-encoded *logical filename*. Never decode a second time and never use `QUrl`, URL parsers, native filesystem paths, or OS-dependent normalization.
- Example: a VFS logical component `résumé%FF` appears as `/résumé%FF` in UTF8 mode and `/r%C3%A9sum%C3%A9%25FF` in ASCII_ESCAPED mode. The decoded logical label remains `résumé%FF`; it is not interpreted as raw filesystem byte 0xFF.
- If **either** local effective preferences or the remote contact's current preferences disable Unicode, only ASCII_ESCAPED is sent and accepted for names, paths, catalog labels and filename metadata governed by this feature. The server chooses the effective mode; client cannot demand UTF-8 when either side disallows it. With both sides permitting Unicode, the connection uses UTF8 by default. Format is still explicitly tagged on each `VirtualPath`; never use host OS, locale, device name patterns or filesystem capabilities to infer it.
- All returned full paths and directory-entry names use the same mode as the request/connection; the `VirtualPath` on a response carries its own explicit tag and must match. Components in `DirectoryEntry.name` (see below) are tagged to avoid ambiguity even without a full path. Opaque node IDs may be supplied for follow-up but do not replace path semantics or current authorization checks.
- Prohibit native path handling on the server boundary. A canonical path resolves component-by-component in the *effective virtual filesystem*, with private mount context and authorization; errors intentionally merge inaccessible and nonexistent objects. Paths can be rebuilt after virtual rename/move from returned entries; opaque IDs avoid accidental source/mount conflation.

## 3. Common data objects and privacy

Sketch (tags allocated during protobuf implementation):

```proto
enum FileShareStatus {
  FILESHARE_STATUS_UNSPECIFIED = 0; // invalid response
  FILESHARE_OK = 1;
  FILESHARE_DENIED_OR_NOT_FOUND = 2;
  FILESHARE_RECURSION_REFUSED = 3;
  FILESHARE_INVALID_REQUEST = 4;
  FILESHARE_STALE_CURSOR = 5;
  FILESHARE_BUSY = 6;
  FILESHARE_UNSUPPORTED = 7;
  FILESHARE_CHANGED = 8;
  FILESHARE_INTERNAL_ERROR = 9;
}
enum VirtualEntryType {
  VIRTUAL_ENTRY_TYPE_UNSPECIFIED = 0;
  VIRTUAL_FILE = 1;
  VIRTUAL_DIRECTORY = 2;
}
enum VirtualEntryState {
  VIRTUAL_ENTRY_STATE_UNSPECIFIED = 0;
  VIRTUAL_ENTRY_READY = 1;
  VIRTUAL_ENTRY_UPDATING = 2;
}
message VirtualName {
  VirtualPathEncoding encoding = 1;
  string value = 2;                 // one component, same canonical rule
}
message DirectoryEntry {
  bytes node_id = 1;                // fixed bounded opaque virtual ID
  VirtualName name = 2;
  VirtualEntryType type = 3;
  VirtualEntryState state = 4;
  optional uint64 size = 5;        // regular files only; omission != zero
  optional int64 modified_utc_day = 6; // whole UTC days since UNIX epoch, no precision leak
  bytes sha256 = 7;                 // empty unless currently verified; if present exactly 32 bytes
  // Optional explicitly approved *portable* user operations/capabilities,
  // never raw mode bits, POSIX inode/device, NT attributes or native symlink types.
}
```

- The only externally visible entry types are FILE and DIRECTORY. Do not publish host-native metadata, special-file details, raw symlinks/targets, native error strings, backend names, local paths, hidden counts, physical directory mtimes, catalog generations or fingerprintable host attributes.
- A peer-visible directory `revision` is an opaque value derived only from visible authorized metadata. Redacted/hidden changes must not change visible counts, revisions or notification behavior. Directory sizes are omitted. File times follow existing coarse day-level VFS semantics. Keep a fixed field set and fixed limits independent of OS.
- The **logical filename** is preserved for accepted virtual files even where a native host cannot materialize it directly (`CON`, `AUX`, `COM1`, trailing dot, case conflicts, etc.). All native storage uses opaque/private names and a reversible logical-name mapping; an operating system's native filename restrictions must not silently rename remote visible objects. The current managed-native policy in `file_manager.md` must be reconciled/versioned before implementing writable FileShare operations, rather than weakening only the wire validator.
- Never treat a `VirtualPath`, `VirtualName`, `DirectoryEntry`, or arbitrary URL text as a filesystem path; their bytes are display/lookup data for the VFS resolver.

## 4. LIST: direct page or optional recursive export

```proto
message ListDirectoryRequest {
  VirtualPath directory = 1;
  bool recursive = 2;             // default false: immediate children only
  uint32 page_size = 3;           // nonrecursive only, zero means server default
  bytes continuation = 4;        // nonrecursive only
}
message ListDirectoryResult {
  FileShareStatus status = 1;
  VirtualPath directory = 2;      // only on success, same encoding
  repeated DirectoryEntry entries = 3; // only on successful nonrecursive LIST
  bytes continuation = 4;        // only on successful nonrecursive LIST
  bool complete = 5;             // only on successful nonrecursive LIST
  bytes catalog_transfer_id = 6; // only on successful recursive acceptance
  uint32 catalog_format_version = 7; // only on recursive acceptance
}
```

**Direct nonrecursive LIST**
1. Validate connection, correlation, mode, path, fields and limits. `recursive=false` requires no EXPORT permission; LIST and traversability on the selected directory are sufficient. Resolve current authorized VFS path.
2. Use the VFS paginated listing with locally capped `page_size`, scan work and cursor lifetime. Filter unauthorized children; return authorized entries in deterministic order. A page may be empty with `continuation` present; `complete=false` then. A cursor carries an opaque binding to requester, scope, path, generation, presentation, expiry and operation. It is never reused across connections or new policy generations.
3. A failed lookup/denial returns `DENIED_OR_NOT_FOUND`, with zero entries, cursor and metadata. Invalid cursors return `STALE_CURSOR`; do not reveal whether a child existed behind a denial.

**Optional recursive LIST**
1. The client sets `recursive=true` with a **new request ID**. `page_size` and `continuation` MUST be absent/zero (no page-by-page recursion). The server performs an atomic *admission decision* before scanning: effective LIST+EXPORT at root, allowed RECURSE where applicable, local feature policy, quotas and readiness. Even otherwise authorized recursion may be declined by the server.
2. **On refusal:** return ONLY `status=FILESHARE_RECURSION_REFUSED` with correlation ID; do **not** populate `directory`, `entries`, `continuation`, `complete`, transfer ID, format version, or any enumeration/size/hidden-policy detail. Do not start staging or a generic file offer.
3. **On approval:** build a bounded authorization-filtered `Vfs::exportCatalog` in private temporary storage. It serializes permitted subtree entries, checks every traversal, rejects cycles and enforces entries/bytes/depth/time. The result is complete within per-directory snapshot semantics or fails without publication. Stage, hash, then send `status=FILESHARE_OK`, the catalog transfer ID and format version, and offer exactly that immutable artifact via the existing generic file-transfer protocol. Never emit a partial catalog disguised as complete.
4. If staging fails, return a generic error with no entry data, offer or partial artifact. Revocation cancels affected generation/transfer at a safe boundary. The client may retry `recursive=false` at the same root and manually enumerate specific directory paths. **The server never falls back from denied recursion to a nonrecursive listing in the same request.**
5. Recursive catalog bytes use the existing versioned VFS catalog representation and `validateCatalog`; they are ordinary content bytes, not a second control-frame sequence. The catalog itself records its effective presentation/format version so a receiver validates tags and ASCII policy before showing anything. The completed transfer proves integrity, not access to later file contents.

**Result sum-type invariants:** status != OK implies all data/continuation/transfer fields absent/default; OK + nonrecursive implies no catalog fields; OK + recursive implies no inline entries/cursor/`complete`. Invalid combinations are protocol errors, not guessed fallback forms. The response's request correlation never doubles as an authorization token.

## 5. STAT and authorized download

```proto
message StatVirtualRequest {
  VirtualPath path = 1;
  bytes known_node_id = 2;       // optional check; not permission
}
message StatVirtualResult {
  FileShareStatus status = 1;
  DirectoryEntry entry = 2;     // success only
  VirtualPath path = 3;         // success only
}
message RequestDownload {
  VirtualPath path = 1;
  bytes known_node_id = 2;       // optional stale-identity fence
  bytes expected_sha256 = 3;     // optional, exactly 32 bytes
  optional uint64 expected_size = 4;
}
message RequestDownloadResult {
  FileShareStatus status = 1;
  bytes transfer_id = 2;        // success only, exactly 16 bytes
}
```

- STAT requires current authorization for path traversal and inspection. Return only portable metadata about the requested visible node. If a node ID is provided alongside path, both must refer to the same authorized node; mismatch is `CHANGED` or `DENIED_OR_NOT_FOUND`, without source/mount hints.
- GET requires current READ/DOWNLOAD for the same mount context and regular file. If expected SHA-256/size is supplied, refuse mismatched immutable content rather than silently serving newer bytes. Do not use `RequestDownload` for upload, native path materialization or arbitrary blob fetch.
- On admission, the server prepares one bound VFS reader and immutable content identity, then creates an ordinary outgoing `FileOffer` with that transfer ID through the existing generic engine. Downloads inherit the existing checkpoint/resume/session/deadline/cancellation rules. Reauthorize subsequent reads and resumption. `FILESHARE_OK` does not mean the recipient has accepted the FileOffer or completed the transfer.
- The accepted transfer's `FileOffer.name` is a bounded, possibly escaped safe download suggestion; the full remote VFS logical name/path remains available in FileShare metadata and cannot be overwritten by host-specific materialization.
- Feature purpose is **trusted local context**: ordinary FileShare content may be chargeable in the later exchange phase; catalog exports, thumbnails and previews are exempt regardless of peer-specified strings. No credit ledgers implemented at this initial protocol stage.

## 6. Cancellation, subscriptions, and protocol resources

```proto
message CancelFileShareRequest {
  uint64 request_to_cancel = 1; // same authenticated connection only
}
message CancelFileShareResult {
  FileShareStatus status = 1;
}
```

- Cancellation only applies to a pending *FileShare request* (e.g., catalog generation), not a previously accepted generic transfer, whose own FileCancel/pause lifecycle is authoritative. Idempotently cancel request work without exposing whether a hidden request existed. Late responses do not reactivate cancelled work.
- Live directory subscriptions and notifications are **optional for initial release**: if implemented, use a separately negotiated sub-capability or documented message types with bounded leases, revocation, visible-only digest and request correlation. Do not opportunistically invent an unbounded notification bus. Direct LIST and recursive catalog transfers work without subscriptions.

All FileShare-specific requests have finite bytes, queue slots, expiries and concurrency. Initially use existing `dsvfs::Limits`: at most 100 entries per direct page, 64 max virtual depth, 30-second ordinary request deadline, max 2 active generated/retained catalog exports globally (subject to per-contact/identity limits), max 1,000,000 export entries and 256 MiB export size. The wire bound may be **stricter** than an internal VFS limit; an operator cannot turn off hard caps. Distinguish default values from actual local admission controls. Check raw lengths before decoding, hashes, lookup, recursive traversal, allocations and expensive hashing. Never include native OS text in refusal or error details.

## 7. Implementation and tests

- Add feature envelope variants, unique protobuf tags, codec validation, negotiated `fileshare-v1`, and bounded request dispatch under current v3. Update `proto/darkspeak.proto` during implementation, **not** by adding an ad hoc parallel schema. Both sides validate enums and required fields (proto3 presence via `optional` where absence differs from zero).
- Route FileShare requests to the contact/identity-scoped feature service, which invokes the existing VFS and transfer manager. Peer should only validate and route messages; no server storage logic in `Peer`.
- Tests: invalid/missing path encoding, invalid UTF-8, mixed ASCII policies, noncanonical `%HH`, escaped slash/double decoding, percent-literal and Unicode round trips, `CON`/`COM1` logical names independent of host, path traversal, source-ID guessing, encoded lengths/cursors, cross-identity node IDs, hidden child filtering, zero-data recursive refusals, per-directory follow-up LIST, catalog-stage failure without partial offer, immutable catalog transfer and SHA-256, stale/revoked GET, and no native-OS fingerprints in metadata or errors.
- Use existing isolated two-instance private-Tor test MCP infrastructure, event-driven assertions and application-wide resource metrics when available; keep tests finite and scoped. Do not create a new transport or standalone FTP network listener.

**No legacy protocol compatibility. No remote write/upload opcodes in this version.** Extend only after the separate upload/exchange specification is approved.
