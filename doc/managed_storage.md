# DarkSpeak managed file and media storage

DarkSpeak uses `DSFileManager::FileManager` for transfer payloads, imported outgoing files, and persisted avatars (identities, contacts, and connection-request notifications). The application database contains references and transfer/UI metadata, not media bytes. Settings, SQLite databases, logs, certificates/vault state, application resources, and explicit local import/export endpoints are outside the managed filesystem.

Application schema 2 adds `file.storage_namespace`, `storage_id`, `pending_id`, and `mime_type`, the bounded namespace registry `managed_namespace`, avatar references/ownership in `media_asset`, and durable deletion intents in `storage_gc`. The schema fixture is `tests/fixtures/managed-storage-v2.json`. Version-1 file paths are not adopted, and old identity/contact avatar blobs are cleared on upgrade. Legacy payloads on disk are left untouched; they are not opened, imported, or deleted through their former database paths. Files must be sent again if required.

## Namespaces and settings

* Downloads use `<downloadLocation>/Managed-v1`. `storage/originalFilenames` defaults to true; false chooses opaque physical names. The choice is persisted in each namespace and is fixed after creation. Both policies retain logical filenames in the catalog.
* Outgoing snapshots and avatars use `<database directory>/managed-private-v1`, with opaque physical filenames.
* `includeConversationNameInDownloadPath` enables stable logical `conversation-<database ID>` directories. This keeps peer/display names out of physical-path construction; logical directories work with either naming policy.
* Storage settings apply after restart. Changing download location registers another namespace, retaining access to previously managed IDs and pending transfers through their original namespace. At most 32 namespaces may be registered/opened. Missing, inconsistent, locked, overlapping, or unsupported roots fail explicitly rather than being silently replaced or imported. Namespace registry entries are private application metadata.
* Receive namespace limits derive from the existing receive-size, unfinished-byte, disk-byte, active-transfer, and interrupted-transfer settings. Limits cannot exceed the namespace total. Private storage uses the finite library defaults. Lowering limits blocks growth, without deleting existing content. Existing application per-contact offer/active-transfer and free-space checks remain in force.

## Transfers, durability, and recovery

Outgoing selection is an explicit local import: open a regular non-symlink source on a bounded worker executor, check size and identity, stream bounded chunks into staging, verify the source did not change, and commit. Persist reserved IDs before copying bytes. Offer the committed snapshot's SHA-256, size, and detected MIME type only after checking current peer preferences. Subsequent transfers and retries read the managed snapshot; changes to the original source do not change it.

Incoming offers require a valid SHA-256. Before accepting, create or reopen a persistent pending writer and save its namespace/file/pending references. Resume from an acknowledged, contiguous storage-valid prefix, rounded down to the protocol chunk boundary. Never infer coverage from file length. Receive buffers and write queues are bounded by the negotiated window. Coalesce available chunks into bounded writes/checkpoints; send cumulative acknowledgements only after the library checkpoint and application progress update. Do not acknowledge the final chunk until `commit()` verifies complete content and the entire expected digest and publishes it. Integrity errors leave staging unpublished and resumable/abortable.

A cancelled operation cannot hide a known publication. A successful incoming commit becomes DONE even if cancellation raced its final publication. A library indeterminate outcome retains its durable operation evidence and pending/application references; do not start a new create as a blind retry. Namespace recovery runs before normal access. File loading repairs application references when library recovery has already completed publication but the application database still records the pending ID. An interrupted outgoing import that did not publish must be selected again; its transient source path is deliberately not persisted.

Deletion triggers retain namespace/file/pending references in `storage_gc` across application row deletion and restarts. Media ownership tombstones follow identity/contact/notification deletion. Bounded cleanup aborts pending content and removes published content through the library, retrying Busy/storage failures later. On startup and periodically, a bounded sweep also aborts reservations that never obtained an application reference; the library protects live writers with Busy. Cleanup never operates on legacy physical paths or adopts orphan filesystem objects.

Avatar PNGs use managed, checksum-verified pending publication and stable replacement IDs. Updates for a key are serialized/coalesced; stored blobs are size-bounded, and restored avatars have bounded dimensions. Model updates use guarded QObject lifetimes. Failed storage operations report a localized status and keep typed diagnostic codes free of names/paths/content.

## External applications and shutdown

Opening a file or its folder explicitly exports a verified temporary copy, then launches the external application/file manager. It does not disclose a managed physical path or make edits to the copy affect stored content. Export copies are owner-only, session-lived, limited collectively to 32 files / 64 GiB (including in-flight reservations), and removed on normal exit. Import/export disk work uses at most two local workers and 32 outstanding jobs; managed operations use the library executor and namespace admission limits.

After the UI event loop ends, a Qt cleanup dispatcher awaits queued media saves and asynchronous library shutdown while execution contexts and the database remain alive. The library checkpoints live pending writers and releases ownership/handles. Offline/online transitions do not close managed storage. Forced process termination relies on the library's persisted checkpoints and recovery protocol. No secure-erasure guarantee is made.

The current native backend supports Linux only and requires the filesystem/SQLite capabilities documented in `src/filemanager/README.md`. CMake builds/fetches QCoro; `DSFM_QCORO_SOURCE_DIR` supports offline sources. Flatpak supplies QCoro sources and uses the KDE SDK's existing Qt/SQLite facilities. No additional Qt module is introduced.

MCP impact: this is an internal storage migration; the repository has no existing MCP file/media API to update. It adds no MCP tools or permissions.

## Verification

Build the application and integration tests, then run `managed_storage`, `protocol_v3`, `ui_history_model`, `ui_components`, `ui_preferences`, `event_journal`, and `qml_smoke`. `managed_storage` exercises snapshot import/export, hidden staging, resume, checksum failure, empty files, reference repair, schema fixtures, cleanup, cancellation, namespace reopening/location changes, avatar restore/edit races, ownership cleanup, and shutdown waiting for active operations. The standalone library retains its own fault-injection and process-death recovery suite. Network peers remain untrusted; a real Tor transfer between application instances is a separate integration exercise.
