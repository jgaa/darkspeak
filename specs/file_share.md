Status: Obsolete
*Replaced by individual specs*

# DarkSpeak FileShare Feature Specification

## 1. Overview and Goals

FileShare adds persistent, permission-controlled file sharing to DarkSpeak.

The feature is inspired by traditional private FTP/file-sharing systems such as War FTP Daemon, but is designed for DarkSpeak's substantially more hostile security model. A remote peer, including an authenticated and approved contact, must always be considered potentially malicious.

FileShare allows a DarkSpeak user to expose selected parts of the local filesystem through a virtual filesystem. Contacts may browse, download, upload, rename, or delete files according to explicitly assigned permissions. Permissions may be assigned directly to contacts or indirectly through groups.

FileShare also supports:

- resumable and persistent download queues;
- upload/download ratios based on bytes or file counts;
- per-user bandwidth limits;
- dedicated upload/drop-box locations;
- duplicate-upload rejection based on SHA-256;
- owner-defined SHA-256 blacklists;
- persistent local file manifests and hash caching;
- stable directory identities;
- efficient handling of copied, renamed, or moved directory trees;
- read-only shares and filesystems;
- privacy-reduced modification timestamps;
- optional transfer auditing.

The primary goals, in order of importance, are:

1. **Security.** A remote peer must not be able to escape the explicitly shared virtual filesystem, obtain unintended permissions, exhaust local resources, manipulate DarkSpeak metadata, or exploit filesystem races.

2. **Privacy.** FileShare must expose only information necessary for the feature. Local paths, precise timestamps, hidden directory activity, filesystem metadata, and unnecessary transfer history must not leak to peers.

3. **Immediate UI responsiveness.** Filesystem scanning, hashing, reconciliation, manifest parsing, database activity, and other potentially expensive work must not block the UI thread.

4. **Low CPU and I/O overhead.** Unchanged files should not repeatedly be hashed, manifests should not repeatedly be parsed, and directory trees should not repeatedly be scanned merely because users browse them.

5. **Efficient operation on large shares.** Renaming, moving, or copying directory trees containing hundreds of gigabytes or millions of unchanged files should not inherently require those files to be rehashed.

6. **Simple, auditable behavior.** Authorization, filesystem resolution, accounting, indexing, directory identity, and transfer handling should remain separate components with well-defined responsibilities.

The existing DarkSpeak file-transfer and resume mechanisms shall be reused for FileShare transfers. FileShare adds discovery, authorization, indexing, accounting, queuing, and management around that mechanism rather than defining a second transfer implementation.


# 2. Security Model

The normal DarkSpeak hostile-peer assumptions apply fully to FileShare.

An authenticated contact is not trusted.

All values received from a peer, including paths, filenames, hashes, offsets, directory identifiers, pagination parameters, transfer requests, and other metadata, are untrusted.

No peer-controlled value may directly determine:

- a local filesystem path;
- an allocation size;
- an unbounded loop;
- an unbounded directory listing;
- a recursive filesystem operation;
- an expensive hash operation;
- the number of persistent objects;
- queue size;
- bandwidth consumption;
- or an authorization decision.

Every such value must first pass explicit local validation and bounds.

Parsing or validation failures must fail safely and must never result in partial authorization.


## 2.1 Filesystem confinement

Remote peers never operate on local absolute paths.

All remotely visible filesystem operations use a virtual namespace and are resolved locally to configured filesystem locations.

For example:

```text
Local:
/srv/archive/linux/

Virtual:
/linux/
```

The peer sees only `/linux/`.

`/srv/archive/linux/` must never be exposed through FileShare.

A peer must not be able to escape a configured root through:

- `..`;
- absolute paths;
- symbolic links;
- filesystem aliases;
- mount points where prohibited by policy;
- malformed path separators;
- path-normalization ambiguities;
- concurrent rename operations;
- or filesystem TOCTOU races.

Filesystem operations should use platform facilities that permit safe root-relative resolution. On Linux, descriptor-relative operations and facilities such as `openat2()` should be considered where appropriate.

Lexically checking a pathname and subsequently opening that pathname is not by itself sufficient confinement.


## 2.2 Metadata namespace

DarkSpeak metadata files are never part of the remotely visible filesystem.

A remote contact must not be able to list, read, create, replace, rename, or delete DarkSpeak metadata regardless of the contact's FileShare permissions.

This restriction must be enforced centrally by the FileShare filesystem/path layer rather than independently by individual operations.


# 3. Virtual Filesystem

Each contact sees a virtual filesystem derived from the shares, groups, and permissions assigned to that contact.

The virtual filesystem maps virtual paths to local filesystem directories.

A configuration might produce:

```text
/
├── Linux/
├── Videos/
├── Documents/
└── Uploads/
```

while these directories may physically reside on unrelated filesystems.

The physical filesystem layout is an implementation detail and must not be visible remotely.


## 3.1 Per-contact VFS

A contact's effective virtual filesystem should be constructed in memory when the contact first starts using FileShare.

The VFS is derived state and is not authoritative persistent state.

It should contain primarily:

- virtual paths;
- effective directory identities;
- backing-share references;
- effective permissions;
- privacy-reduced directory modification dates;
- and information required for efficient authorization and navigation.

Individual file entries should generally not be copied into the per-contact VFS. They should be obtained from the corresponding parsed directory manifest when a directory is actually listed.

This avoids creating millions of duplicated in-memory file objects merely because a contact opens FileShare.


## 3.2 VFS lifetime

A per-contact VFS may be cached while useful, for example while:

- the contact is actively browsing;
- FileShare transfers are active;
- queued transfers depend upon it;
- or another local FileShare operation needs it.

Unused VFS instances may be discarded.

They must always be reconstructable from authoritative local configuration, directory topology, manifests, and directory identity overrides.

A VFS must not be persisted and reused blindly across application restarts.


# 4. Shares

A Share defines a virtual filesystem root backed by a local filesystem location.

A Share has a stable opaque identifier independent of its display name or physical path.

Conceptually:

```text
Share
    id
    display_name
    local_root
    virtual_path

    permissions
    bandwidth_policy
    ratio_policy
    upload_policy
    audit_policy
```

Changing the display name must not change its identity or require contacts to be reassigned.


# 5. Permissions

FileShare supports at least:

- list;
- read/download;
- upload;
- write;
- rename;
- delete;
- recurse;
- deny.

`upload` and `write` are distinct.

`upload` permits submission of a new file into an allowed destination without necessarily granting the ability to browse, modify, rename, retrieve, or delete files already present there.

`write` represents broader modification of a visible filesystem namespace.


## 5.1 Recursion

`recurse` controls whether a rule applies to descendants of the directory where the rule is defined.

It is not itself an independent filesystem operation.


## 5.2 Effective permissions

Permissions may come from:

- direct contact assignments;
- one or more group memberships;
- and multiple applicable share/path rules.

Allow permissions are additive where rules overlap.

**Deny always wins.**

A denied path must not become accessible merely because another group or direct assignment grants access to the same path.

Permission resolution must have precisely defined path-specific precedence. More-specific path rules must be evaluated consistently when deriving effective permissions.

The same authorization engine must be used for actual remote requests and local permission-preview functionality.


# 6. Groups

Contacts may be organized into FileShare groups.

A group has:

- a stable opaque random identifier;
- a mutable display name;
- zero or more members;
- zero or more Share/permission assignments.

Renaming a group does not change its identity.

A contact may simultaneously:

- belong to multiple groups;
- have directly assigned permissions;
- and receive effective permissions from all applicable assignments.


# 7. Directory Identity and Topology

Directories have stable **effective identities** independent of their current pathname.

Conceptually:

```text
DirectoryNode
    effective_directory_id
    parent_id
    name
    manifest_reference
```

Directory topology and file contents are deliberately separate concepts.

This allows a directory to be moved or renamed without invalidating hashes for the files contained within it.


## 7.1 Embedded `.dirid`

When possible, DarkSpeak should place a small metadata file in writable indexed directories:

```text
.darkspeak-dirid
```

It contains a stable random directory identifier.

This allows DarkSpeak to recognize a directory that was renamed or moved while DarkSpeak was not running.

A `.dirid` is an indexing hint only.

It:

- is not an authorization token;
- does not grant access;
- must not determine permissions;
- and must never be remotely visible.

The `.dirid` should contain enough information to identify its format/version and directory identity. It may also be signed by the local DarkSpeak identity.


## 7.2 Embedded identity versus effective identity

An important distinction exists between:

```text
embedded_directory_id
```

and:

```text
effective_directory_id
```

Normally they are identical.

However, directories can be copied. A copied directory may contain an exact copy of:

- `.darkspeak-dirid`;
- its embedded manifest;
- and all files.

Two physical directories can therefore legitimately present the same valid embedded directory ID.

DarkSpeak must not assume that identical `.dirid` values mean that two simultaneously existing directories are the same physical directory.

The local DarkSpeak directory registry determines the **effective identity** used internally and by FileShare.

Embedded metadata provides the normal persistent identity hint, but the local registry is authoritative when identity conflicts exist.


# 8. Directory Identity Conflicts

Duplicate embedded directory IDs are expected filesystem events and must be handled automatically.

For example:

```text
/a/linux
/b/linux-copy
/c/archive/linux
```

may all contain:

```text
embedded_directory_id = X
```

after a directory was copied while DarkSpeak was offline.

DarkSpeak must assign distinct effective identities to simultaneously existing copies.

Under no circumstances may duplicate `.dirid` values cause:

- directories to be silently merged;
- permissions to migrate from one location to another;
- manifests from separate copies to be treated as one mutable directory;
- or remote access to be expanded.


## 8.1 Deterministic conflict resolution

Conflict resolution must not depend on filesystem enumeration order.

The preferred precedence is:

1. An existing persisted identity override/mapping wins.
2. If DarkSpeak previously knew one location as the owner of the embedded ID and that location still exists, that location retains the original effective ID.
3. Otherwise, DarkSpeak selects a deterministic winner among newly discovered copies.
4. Every losing copy receives a distinct newly generated effective directory ID.

Where no stronger historical evidence exists, normalized canonical local pathname ordering may be used as the deterministic tie-breaker.

The ordering is not considered evidence about which copy is historically original. It exists only to ensure repeatable behavior.


## 8.2 Writable losing copies

If a losing copy is writable, DarkSpeak should normally repair the conflict physically.

For example:

```text
/a/linux
    embedded X
    effective X

/b/linux-copy
    embedded X
    effective Y
```

DarkSpeak may rewrite the second directory's:

```text
.darkspeak-dirid
.darkspeak-manifest.json
```

using the newly generated ID `Y` and re-sign the metadata.

This does not require file contents to be rehashed merely because the directory identity changed.

Once the on-disk metadata has been repaired, a permanent identity override is normally unnecessary.


## 8.3 Read-only losing copies

If a conflicting directory is read-only, DarkSpeak must not require filesystem modification to resolve the conflict.

For example:

```text
/a/linux
    embedded X
    effective X

/mnt/readonly/linux
    embedded X
    effective Y
```

The second mapping is retained in DarkSpeak's private local database as a directory identity override.

The read-only filesystem remains unchanged.


## 8.4 Writability does not determine ownership

Whether a directory is writable must not by itself determine which copy retains the original identity.

If DarkSpeak already knows:

```text
/original -> X
```

then `/original` retains `X` even if it is now read-only and a newly discovered copy is writable.

The writable copy receives a new ID.

Likewise, if the established original is writable and the copy is read-only, the original retains `X` and the read-only copy receives a local effective-ID override.

This minimizes unnecessary identity churn.


## 8.5 All-new conflicts

If DarkSpeak has no previous authoritative mapping and discovers multiple copies simultaneously, the deterministic tie-breaker selects one to retain the embedded ID.

Writable losing copies should receive newly generated `.dirid` values.

Read-only losing copies receive persistent local overrides.


# 9. Directory Identity Override Registry

DarkSpeak maintains a persistent local registry for directory identity exceptions.

Conceptually:

```text
DirectoryIdentityOverride
    local_path
    observed_embedded_dirid
    effective_dirid
    optional filesystem identity hints
```

Local paths and filesystem identity information in this registry are private implementation data and are never remotely exposed.

The registry is authoritative when an override exists.


## 9.1 Read-only directory moves

A path-based override has an unavoidable limitation: a read-only conflicting directory may itself be moved while DarkSpeak is offline.

The registry may therefore retain additional local identity hints where available, such as:

- filesystem/device identity;
- inode or equivalent file ID;
- embedded directory ID;
- manifest identity/signature/hash.

These values are reconciliation hints only.

They must never grant FileShare authorization.

If DarkSpeak cannot confidently associate a moved read-only directory with a previous override, it must prefer safe deterministic conflict resolution over guessing.

DarkSpeak must not recursively hash an entire directory tree merely to determine which identity override probably belongs to it.


# 10. In-Memory Identity Override Cache

Directory traversal must not perform a database query for every discovered directory.

At startup, the persistent directory identity override registry is loaded into an in-memory fast lookup table before FileShare reconciliation begins.

Normal reconciliation and VFS operations consult the in-memory representation.

Conceptually:

```text
OverrideKey
    canonical local path
    optional filesystem identity hints

        ->

OverrideValue
    observed embedded dirid
    effective dirid
```

The exact lookup representation is an implementation detail, but normal identity resolution should be approximately constant-time and must not require synchronous database I/O.


## 10.1 Cache consistency

The database and in-memory identity override cache represent one logical state.

Mutations must be serialized through the directory identity registry.

An override must not be published to normal readers unless its corresponding persistent database mutation has successfully committed.


## 10.2 Atomic logical updates

A typical update is:

```text
request identity change
        |
        v
begin database transaction
        |
        v
insert/update/delete override
        |
        v
commit database transaction
        |
        v
publish corresponding in-memory state
        |
        v
invalidate/update dependent derived state
```

Readers within the running process must not observe an intentionally inconsistent committed state between the database and the published lookup table.

A short exclusive synchronization region or atomic publication of a newly prepared immutable cache/snapshot may be used.

Identity overrides are expected to change infrequently, so implementation simplicity and correctness take precedence over sophisticated lock-free mutation.


## 10.3 Crash behavior

If DarkSpeak terminates before the database transaction commits, the override has not been persistently changed.

If DarkSpeak terminates after the database commit but before the in-memory publication completes, the next startup reconstructs the in-memory cache from the committed database.

The persistent database is therefore the recovery authority across process restarts.


## 10.4 Override generation

The identity override registry should maintain a monotonically increasing generation.

Conceptually:

```text
directory_identity_generation = N
```

Derived structures may record the generation against which they were constructed.

When directory identity mappings change, dependent state must be invalidated or refreshed before it can be relied upon for FileShare authorization or remote visibility.

This includes affected:

- directory graph state;
- manifest mappings;
- global hash-index references;
- per-contact VFS instances.


# 11. Directory Manifests

DarkSpeak maintains a manifest for indexed files in each directory.

Manifests exist primarily to avoid repeated filesystem inspection, JSON parsing, and expensive SHA-256 calculation.

A directory manifest contains **files only**.

Child directories belong to the directory topology/index rather than the file manifest.

This makes directory moves and renames inexpensive.


## 11.1 Manifest contents

A manifest should conceptually have three top-level elements:

```json
{
    "signature": "...",
    "last_update": "...",
    "manifest": {
        "directory_id": "...",
        "generation": 123,
        "files": []
    }
}
```

Each regular-file entry should contain at least:

- filename;
- size;
- highest useful local `mtime` precision;
- SHA-256.

Additional local filesystem metadata may be retained when useful for reliable cache invalidation.


## 11.2 Manifest signatures

The signature must cover all security-relevant manifest information, including `last_update`.

The signature should cover a canonical representation of the document with the signature field itself omitted.

Because arbitrary JSON serialization is not canonical, signed manifests must use a precisely defined canonical JSON representation.

Duplicate object keys and malformed/noncanonical values must be rejected.


## 11.3 Manifest identity after a copied-directory conflict

A copied read-only directory may contain a valid signed manifest referring to its original embedded directory ID even though DarkSpeak has assigned the physical copy a different effective directory ID.

DarkSpeak must not modify the parsed signed document in memory and then treat its existing signature as valid.

Instead, directory state distinguishes between:

```text
embedded_directory_id
effective_directory_id
manifest_source
```

The original manifest may remain valid evidence about the indexed file contents under its embedded identity.

The local directory registry determines the effective identity used by FileShare.

If updated metadata must later be generated for a read-only copy, DarkSpeak stores the updated manifest in private state using the effective directory identity.


## 11.4 Manifest placement

Where possible, manifests should live in their corresponding actual filesystem directories.

For example:

```text
Linux/
    .darkspeak-dirid
    .darkspeak-manifest.json
    debian.iso
    fedora.iso
```

This allows a directory moved while DarkSpeak is offline to carry both its identity and cached file information.

Embedded manifests are not required.

For read-only mounts, restricted directories, unusual filesystems, or directories where metadata cannot be written, DarkSpeak stores equivalent metadata in its private application state.

The remainder of FileShare must not depend on which storage method is used.

A directory may transition between writable and read-only states without invalidating FileShare.


# 12. Manifest Cache

Repeated parsing and verification of JSON manifests during browsing is undesirable.

DarkSpeak should maintain an in-memory cache of parsed manifests.

Conceptually:

```text
DirectoryId
    ->
shared immutable DirectoryManifest
```

The parsed C++ representation should provide efficient access to commonly needed data.

A manifest may contain indexes such as:

```text
filename -> file entry
SHA-256  -> file entry/reference
```

Manifest objects should preferably be immutable after admission to the cache.

When a manifest changes, a new parsed object can replace the old cache entry while existing readers safely retain references to the old object until finished.


## 12.1 LRU behavior

The manifest cache should be bounded and use an LRU or comparable eviction strategy.

Cache size is an implementation/configuration detail.

Eviction must not invalidate an object currently being used by an operation.

The purpose is to make normal browsing involve memory lookups rather than repeated disk access and JSON parsing.


# 13. File Hashing and Change Detection

SHA-256 is the canonical FileShare content hash.

Hashes are computed locally and cached in manifests.

DarkSpeak should avoid hashing unchanged files repeatedly.

For change detection, implementations should use the strongest practical combination of local filesystem metadata, potentially including:

- filesystem identity;
- device/inode or equivalent;
- size;
- highest-resolution `mtime`;
- change metadata where useful.

If relevant metadata indicates that a file changed, its hash must be recalculated.

The hash cache assumes the local filesystem and local OS are within the local trust boundary. It is not intended to prevent a malicious privileged local process from deliberately changing file contents while forging filesystem metadata.


## 13.1 Moves, renames, and copies

Renaming or moving an unchanged indexed file or directory should reuse already known SHA-256 information whenever the local index can reliably identify the object.

Moving a large unchanged directory tree must not by itself require rehashing all contained files.

Copying a directory with a valid manifest can also provide useful cached hash information. However, copied metadata must first pass directory identity conflict resolution and filesystem metadata validation before the cached hashes are trusted as current for the copy.


# 14. Global Hash Lookup

DarkSpeak should maintain an efficient in-memory lookup from SHA-256 to known indexed files.

Conceptually:

```text
SHA256 -> known file references
```

This index is updated incrementally when manifests change.

It supports:

- duplicate-upload detection;
- blacklist checking;
- content lookup;
- and future content-based FileShare functionality.

It must not require scanning or parsing every manifest for each lookup.


# 15. Duplicate Upload Policy

FileShare may optionally reject uploads whose SHA-256 already exists.

Useful policies include:

- allow duplicates;
- reject duplicates already present in the target area;
- reject duplicates present anywhere in the owner's FileShare index.

The sender may announce the expected SHA-256 before transfer so that an obviously unwanted duplicate can be rejected without transferring its contents.

The sender-provided hash is never authoritative.

A completed upload must be locally hash-verified before being committed as successfully received.

A rejected duplicate earns no ratio credit.


# 16. Hash Blacklists

The owner may maintain SHA-256 denial lists.

If an announced upload hash is blacklisted, DarkSpeak should reject the upload before accepting its payload.

After transfer, the actual computed SHA-256 must still be checked before the file is committed.

Blacklists should support at least:

- global FileShare blacklist;
- per-Share blacklist.

Contact/group-specific blacklists are not required initially.


## 16.1 Importing hashes

The UI should allow hashes to be:

- entered manually;
- pasted as text;
- imported from text files.

Input should intentionally use a simple format.

DarkSpeak may accept:

- one SHA-256 per line;
- whitespace-separated SHA-256 values;
- common SHA-256 checksum-list formats.

Only valid SHA-256 hashes are relevant. Supplied filenames and comments must not influence filesystem operations.

Hash-list parsing must have explicit limits on:

- input size;
- number of hashes;
- line length;
- and memory consumption.

Hash lists must not support remote includes, URLs, scripts, recursive imports, or other active functionality.


# 17. Modification Times and Privacy

DarkSpeak retains high-resolution modification times internally because they are useful for efficient change detection.

Those timestamps must not be exposed remotely at native filesystem precision.

Remote modification timestamps should be privacy-reduced.

The default is **whole UTC days**.

An implementation may support policies such as:

- hidden;
- day;
- hour.

Full-resolution filesystem timestamps are not a FileShare remote-display option.


## 17.1 Directory content date

The remotely meaningful date for a directory is not its filesystem directory `mtime`.

Instead, it represents the newest visible file content beneath that directory.

A directory therefore appears to have the same privacy-reduced date as the newest file visible to that contact within its accessible subtree.


## 17.2 Hidden-content side channels

Files or directories inaccessible to a contact must not influence timestamps shown to that contact.

Activity under a denied subtree must not cause a parent directory's visible date to change for a contact unable to access that subtree.


## 17.3 Efficient propagation

Directory nodes may maintain cached newest-content timestamps.

When file content changes, timestamp changes can propagate toward ancestors until no effective value changes.

Because remote timestamps normally have day precision, per-contact VFS propagation may stop once the visible day is unchanged.


# 18. Upload Areas

FileShare supports dedicated upload destinations.

A configured upload root should automatically isolate uploads by contact identity.

Conceptually:

```text
<upload-root>/<contact-id>/
```

The peer does not choose or control the `<contact-id>` component.

The local UI may display the contact's nickname or locally assigned name rather than exposing identity IDs unnecessarily.


## 18.1 Upload-only access

A contact may be permitted to upload without being permitted to:

- list existing uploads;
- download them;
- rename them;
- delete them;
- or know what other files exist.

This provides drop-box semantics.


## 18.2 Upload collisions

Remote uploads must not silently overwrite existing files.

The default operation is create-new-only.

DarkSpeak may internally store incoming files using a transfer/content identifier while retaining the peer-provided filename as metadata until the owner chooses a final filesystem name.


# 19. Ratios and Accounting

FileShare supports traditional upload/download ratios.

Ratio accounting may operate on:

- number of files;
- number of bytes;
- or be disabled.

Examples:

```text
1 uploaded file -> 1 downloadable file
1 uploaded MiB  -> 10 downloadable MiB
```

Authorization and accounting remain separate.

First:

> Is this contact authorized to access this object?

Then:

> Does the configured ratio policy currently permit the transfer?


## 19.1 Persistent accounting

DarkSpeak should persist at least:

- uploaded files;
- uploaded bytes;
- downloaded files;
- downloaded bytes;
- applicable credit state.

Accounting is keyed to the authenticated DarkSpeak contact identity.

A peer's claim about its previous transfer totals or available credit is never authoritative.


## 19.2 Upload credit

Upload credit is granted only after the complete file has been received and its SHA-256 verified.

Rejected, invalid, duplicate, blacklisted, or incomplete uploads do not earn full upload credit.


## 19.3 Downloads and resumed transfers

Accounting must operate correctly across partial and resumed transfers.

Credit consumption must correspond to data actually transferred according to the chosen accounting mode and must not allow reconnect/resume behavior to duplicate or restore already-consumed credit.

Exact transfer-accounting mechanics may be defined together with the protocol operations.


# 20. Bandwidth Control

FileShare supports bandwidth limits.

Limits may exist at different scopes, including:

- global FileShare limit;
- Share default;
- group/contact limit;
- connection/session limit.

Where several limits apply, the effective rate should not exceed the most restrictive applicable limit.

A contact must not obtain N times its intended bandwidth merely by opening N simultaneous file transfers.


# 21. Directory Listings

Directory listings must be bounded.

A directory containing millions of files must not cause:

- an enormous allocation;
- an enormous protocol response;
- a huge UI model;
- or an unbounded CPU operation.

Directory listings should therefore support bounded pagination.

The server enforces a local maximum regardless of the value requested by the peer.


## 21.1 No server-side recursive listing

FileShare does not initially provide a remote operation meaning "recursively list everything below this directory."

A client traverses bounded directories individually.


# 22. Persistent Download Queue

FileShare downloads are persistent client-side operations.

Users may queue multiple files for download.

Queue state survives:

- temporary disconnection;
- contact unavailability;
- DarkSpeak restart.

The UI should make it possible to:

- resume;
- pause;
- cancel;
- remove;

individual queued transfers.


## 22.1 Resume identity

A queued download must remain bound to the expected file identity.

Queue information should include sufficient immutable information such as:

- peer DarkSpeak identity;
- Share/file identity;
- expected SHA-256;
- expected size;
- virtual/display path;
- local destination;
- received/resume state.

The peer's onion address is not its identity.


## 22.2 Changed remote files

If a queued file changes after a partial download, DarkSpeak must not silently resume using replacement content.

If the expected SHA-256 no longer matches, the resume operation fails as "file changed" or equivalent.


# 23. Rename and Delete

Rename and delete are explicit permissions separate from write/upload.

Remote recursive deletion is not required for the initial FileShare implementation.

Deleting an individual file may be supported.

Directory deletion, if supported initially, should be restricted to empty directories.

A single peer request must not trigger an unbounded recursive delete operation.


# 24. Audit and Transfer History

FileShare may optionally maintain transfer history.

Detailed logging is a privacy decision and should not be required merely for FileShare to function.

Suggested levels are:

### Off

No optional transfer history.

Necessary accounting state may still exist where ratio policies require it.

### Statistics

Retain aggregate information such as:

- contact;
- direction;
- bytes;
- success/failure;
- coarse time where useful.

### Detailed

Additionally retain information such as:

- Share;
- virtual path;
- filename/hash;
- operation.

Detailed history should be explicitly enabled by the owner.

DarkSpeak should not create a permanent forensic history of shared content by default merely because FileShare is enabled.


# 25. Startup Reconciliation

FileShare metadata may become stale while DarkSpeak is not running.

At startup, DarkSpeak reconciles configured shares with manifests and current filesystem state.

Reconciliation must not run on the UI thread.


## 25.1 Worker execution

Startup reconciliation runs in a worker thread at lower priority than interactive/UI work.

DarkSpeak messaging and the rest of the application should become usable normally while FileShare reconciliation continues.

Filesystem activity must also be conservative. Merely lowering CPU thread priority is not sufficient if reconciliation saturates storage.

Initial implementations should favor bounded concurrency and predictable I/O over maximum scanning speed.


## 25.2 Identity registry initialization

Before directory reconciliation begins:

1. the persistent directory identity override registry is loaded from the database;
2. its in-memory fast lookup representation is constructed;
3. the registry generation is established;
4. only then may filesystem identity reconciliation begin.

The reconciliation hot path must not perform a database query for every directory.


## 25.3 FileShare startup state

FileShare remains unavailable to remote peers until startup reconciliation has completed successfully.

Conceptually:

```text
OFFLINE
    |
    v
INDEXING / RECONCILING
    |
    v
ONLINE
```

Remote FileShare requests received while reconciliation is in progress should receive a bounded, well-defined "FileShare not ready" response rather than being queued indefinitely.


## 25.4 Reconciliation work

Reconciliation includes, as necessary:

- discovering directory topology;
- identifying directories using `.dirid`;
- applying existing identity overrides;
- detecting duplicate embedded directory IDs;
- deterministically resolving newly discovered identity conflicts;
- rewriting metadata for writable conflicting copies where appropriate;
- creating persistent overrides for read-only conflicts;
- locating embedded/private manifests;
- validating manifests;
- identifying added, removed, renamed, moved, or copied objects;
- recalculating hashes only where needed;
- rebuilding/updating the global hash lookup;
- updating newest-content timestamps;
- updating internal directory topology.

Moving or copying a large unchanged directory tree should primarily involve topology, identity, and metadata validation rather than content rehashing.


## 25.5 Future optimization

A future implementation may distinguish:

```text
INDEXING
VERIFYING
ONLINE
```

and make unaffected files available while expensive hash verification continues.

This is not required initially.

For the first implementation:

> FileShare remains offline until reconciliation is complete.


# 26. Runtime Filesystem Changes

While FileShare is running, filesystem changes should update manifests and indexes incrementally.

The implementation should avoid rescanning complete shares after every change.

Relevant changes may update:

- directory topology;
- directory identity registry;
- affected manifest;
- SHA-256 index;
- directory newest-content time;
- active per-contact VFS caches.

Filesystem watching is an implementation detail and may vary by platform.

Correctness must not depend solely on receiving every filesystem notification; startup reconciliation remains the recovery mechanism after missed/offline changes.


# 27. Performance Requirements

FileShare is intended to work with large file collections without degrading DarkSpeak responsiveness.

The following are architectural requirements:

- UI operations must not wait for filesystem scanning, hashing, or synchronous database access.
- SHA-256 must not be recalculated for unchanged files unnecessarily.
- JSON manifests should normally be parsed once per cache admission, not once per listing.
- Parsed manifests should be reusable by multiple contacts.
- Per-contact VFS instances should not duplicate complete file manifests.
- Normal directory browsing should primarily operate on cached/indexed information.
- Directory identity resolution on the hot path should use the in-memory override registry rather than database queries.
- Expensive work must be bounded and interruptible.
- Startup reconciliation must run outside the UI thread.
- Background work should favor low interactive impact over maximum throughput.
- Memory caches must be bounded.
- Peer-controlled requests must not force arbitrary cache growth.
- Directory moves, renames, and copies should reuse existing valid hash metadata where safe.
- Read-only filesystems must not require repeated expensive work merely because embedded metadata cannot be updated.


# 28. Suggested UI

FileShare should become a main navigation area in DarkSpeak.


## 28.1 FileShare browser

The main FileShare view should allow the user to browse remote contacts' available virtual filesystems.

Useful information includes:

- filename/directory name;
- size;
- privacy-reduced modification date;
- transfer state;
- available operations.

Directories may be sorted by their newest visible content date.

Downloads may be added to the persistent transfer queue.


## 28.2 Local FileShare administration

The local FileShare area should provide management of:

- Shares;
- virtual mount paths;
- permissions;
- groups;
- upload areas;
- bandwidth policies;
- ratio policies;
- duplicate policy;
- hash blacklists;
- auditing policy.


## 28.3 Groups

The group UI should allow:

- creating/renaming/deleting groups;
- viewing members;
- adding/removing contacts;
- assigning/removing Share permissions.


## 28.4 Contact information

Contact information should gain a local **Sharing** tab.

This view should allow:

- direct Share assignment;
- group membership management;
- removal of assignments;
- viewing effective permissions.


## 28.5 View as contact

The owner should be able to inspect the FileShare namespace exactly as a particular contact sees it.

This uses the real per-contact VFS and authorization engine.

The owner can therefore verify that:

- expected directories are visible;
- denied directories are absent;
- write/upload permissions are correct;
- effective permissions from multiple groups behave as intended.


## 28.6 Startup status

During reconciliation the UI should clearly indicate that FileShare is being updated and is temporarily offline.

Progress information may be displayed where it can be obtained cheaply.

Calculating progress must not itself require expensive preliminary scans merely to produce a percentage.


## 28.7 Identity conflicts

Normal copied-directory identity conflicts should be resolved automatically and should not require user interaction.

The UI may expose diagnostic information about identity overrides and repaired conflicts in an advanced FileShare diagnostics view.

Conflicts that cannot be safely resolved automatically should cause the affected area to remain unavailable rather than guessing.


## 28.8 Transfer queue

The UI should provide a consolidated view of queued FileShare downloads across contacts.

It should make disconnected/paused/running/failed state obvious and allow the user to resume, pause, cancel, or remove transfers.


## 28.9 Catalog exports

Remote directory browsing supports bounded single-directory or recursive authorization-filtered catalog exports, including traversal across virtual mounts. Follow `specs/vfs.md` §6 for staged immutable exports, per-directory consistency, resource limits, current authorization, and optional future cache composition. Transfer exports using the existing generic file-transfer protocol and its byte-window acknowledgements, integrity checks, cancellation, and resume. Remote requests never receive physical filesystem cursors or unbounded recursive access.


# 29. No-Goals

The initial FileShare feature is **not** intended to provide:

- arbitrary remote filesystem access;
- exposure of physical filesystem paths;
- full-resolution remote filesystem timestamps;
- remote shell or filesystem scripting;
- unbounded remote recursive directory listing;
- remote recursive deletion;
- peer-controlled symbolic-link traversal;
- unbounded directory operations;
- automatic remote fetching of hash lists;
- active/scriptable manifest content;
- synchronization of complete directory trees;
- distributed filesystem semantics;
- conflict-free collaborative editing;
- peer authority over local accounting;
- mandatory detailed transfer surveillance;
- a second independent file-transfer implementation.

FileShare is also not intended to defend cached hashes against a privileged malicious process on the local machine deliberately modifying both file content and filesystem metadata.


# 30. Core Architectural Invariants

The implementation must preserve these invariants:

1. **Peers see a virtual filesystem, never the physical filesystem namespace.**

2. **A peer-controlled path is never directly used as a local filesystem path.**

3. **Deny always wins during authorization.**

4. **The same authorization engine powers real access and administrative preview.**

5. **DarkSpeak metadata is invisible and inaccessible through FileShare.**

6. **Effective directory identity is independent of pathname.**

7. **An embedded `.dirid` is an identity hint, not an authorization primitive or unconditional source of truth.**

8. **Simultaneously existing copies of a directory must receive distinct effective directory identities.**

9. **Directory identity conflicts are resolved deterministically and must never depend on filesystem enumeration order.**

10. **The local DarkSpeak directory registry is authoritative when embedded directory identities conflict.**

11. **Read-only filesystems must be fully supported without requiring modification of embedded metadata.**

12. **Directory identity exceptions are persisted in the local database and mirrored in a fast in-memory lookup table.**

13. **Hot-path directory identity resolution must not require database access.**

14. **Persistent identity overrides and the published in-memory override view change as one logical transaction.**

15. **A committed override-registry change invalidates dependent derived state before that state is relied upon.**

16. **Directory topology is separate from per-directory file manifests.**

17. **Moving, renaming, or copying unchanged content does not inherently require rehashing it.**

18. **SHA-256 supplied by a peer is a claim; locally verified SHA-256 is authoritative.**

19. **Hidden content must not affect information visible to unauthorized contacts.**

20. **Full filesystem timestamp precision remains local.**

21. **Persistent manifests/indexes are authoritative cache/index state; per-contact VFS instances are disposable derived state.**

22. **Expensive filesystem and database work never runs synchronously on the UI thread.**

23. **All caches and remote operations have explicit local bounds.**

24. **FileShare remains unavailable until startup reconciliation establishes a trustworthy namespace, directory identity mapping, and index.**

25. **Authorization, accounting, indexing, directory identity, filesystem resolution, and transfer execution remain separate subsystems.**

26. **Correctness must not depend on filesystem notifications being delivered while DarkSpeak is running.**

27. **Privacy and security take precedence over convenience and performance; subject to those constraints, unnecessary CPU, memory, filesystem I/O, database I/O, and latency should be aggressively avoided.**
