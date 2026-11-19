# DarkSpeak — Application-wide Content Transfer Queue

**Status:** Initial architecture and implementation specification. **UI:** `transfer_queue_ui.md`. **Existing contracts:** `protocol.md` (checkpoints/limits), `file_manager.md` (storage), `ftp/fileshare_authorization.md` (authorization/credits), `ftp/fileshare_server.md` (remote FileShare), `media_previews.md` (preview policy). This is an application-wide feature, not an FTP-specific queue. Implementation is a later milestone and must not reopen the completed coroutine transfer refactor.

## 1. Goals and ownership

A single application-wide queue is DarkSpeak's entry point for transferring content **in either direction**. Feature adapters submit transfer intents: conversation/message attachments, profile/bio attachments, image/video previews, authorized directory catalog exports, FileShare content downloads/uploads and future social-media objects. Queueing must preserve interactive responsiveness, conversation priority, privacy/security, correctness across restart, and fair resource sharing. **Do not build a second transfer engine or independent attempt controller.**

Three distinct levels:
- **Feature owner:** knows why an object is being sent/received, current acceptance and authorization, source/destination/content identity, dependency/feature policy and (if relevant) FileShare credits. Only a trusted local adapter assigns purpose and priority class, never a peer's MIME/name/claimed type. Feature-specific accounting does not live in the queue.
- **TransferQueue:** owns persistent work items, priority/reorder policy, eligibility, admission, dependencies, wakeups and user operations. It selects *which transfer to attempt*, not how checkpoints, I/O, publication or cancellation work. It observes session outcomes and journal evidence. The application has **one** instance across identities/contacts, not one queue per contact or FileShare module.
- **Existing TransferManager/TransferSession:** owns active attempt reservation, state transitions, structured coroutine lifetime, checkpoint backpressure, resumable interruption, final hash and publication. The queue invokes its APIs and sends typed requests to the owning session. `Peer` transports validated events; `FileManager` persists content/journal. Neither `Contact` nor UI/feature controllers execute an independent transfer scheduler.

The existing `Contact::fileQueue_` and File-model queue state are migration inputs/projections, not additional long-term scheduling authorities. Changes must preserve conversation attachment manifests, thumbnail/primary relationships and the currently implemented offer/response semantics.

## 2. Durable work-item model

Persist a bounded schema-versioned private queue table with:
- local immutable `WorkId`; linked stable `TransferId` and direction (SEND/RECEIVE); local identity, authenticated remote contact, trusted feature `Purpose` and owning feature's immutable record/reference; optionally a bounded parent/batch ID and dependency IDs.
- Immutable content expectations when known (size, SHA-256, content version/source binding), sanitized presentation name and MIME, the *local* source/destination reference. Never persist arbitrary peer paths as native paths or a reader/writer/coroutine. Identifiers and object metadata remain scoped to that local identity/contact, VFS mount and feature.
- Queue class, user-adjustable priority/order, creation/admission/terminal time, user pause/resume preference, typed wait/pause/failure reason, bounded retry/backoff eligibility, last durable journal linkage and feature-owned accounting/authorization reference (not the ledger itself).
- Work lifecycle and UI state are **projections reconciled with authoritative transfer journal and storage**, not duplicate checkpoint writers. Active session attempts are ephemeral. An item retains its `TransferId` across resumed attempts, each attempt uses new proper nonce as required. Idempotent submission keys prevent duplicate work on reconnect/repeated offers.
- Optional bounded `BatchId` groups related content for presentation and cancellation (e.g. message attachments and their thumbnails), without merging transfer identities, trusted purposes, checkpoints, or per-file acceptance. Use explicit bounded dependencies rather than recursive peer-controlled DAGs. Feature records remain authoritative for whether a batch exists/was accepted.

Transfer items represent **whole immutable content**; do not enqueue every chunk. Catalog generation, preview creation, source hashing and copy staging are optional *bounded preparation work*, never hidden unbounded tasks. An unapproved recursive catalog request produces no content-transfer item; generation failure produces no half-complete transfer item.

## 3. Lifecycle and authorization

States (typed reasons carried separately):
```
Inbound offer -> AwaitingAcceptance -> Queued
Outbound intent --------------------> Queued
Queued -> Preparing -> Ready -> Active -> Completed
                 |          |        -> Paused -> Queued (on eligible resume)
                 |          |        -> Failed / Cancelled
Queued / AwaitingAcceptance ---------> Cancelled / Rejected
```
- `AwaitingAcceptance` must not allocate a pending writer, send an acceptance or consume a bulk slot. Explicit user acceptance or narrowly scoped policy-approved autoaccept is required. An unauthorized/unsupported offer fails closed without enrolling an active attempt. Automatic acceptance of a thumbnail never implicitly accepts its primary.
- User `Pause` is **reversible**, distinct from `Cancel`. Pausing an active attempt requests a controlled session pause at a durable mutually confirmed checkpoint; never mark an unverified prefix as resumable. The owning session may need bounded time to reach a safe boundary; UI displays `Pausing` until actual confirmation. A security revocation or user cancellation has precedence over voluntary pause, closes admission immediately and follows current safe publication semantics. A finished/irreversible commit cannot be undone.
- `Resume` re-enqueues the same work after current authorization, source/content/quotas, contact state and checkpoint validity are checked. No automatic override of an explicit user pause, rejection, cancellation, credit-deficit wait or terminal corruption failure. Disconnect may auto-resume on authenticated reconnection only under feature/local settings and bounded backoff; no busy polling and no repeated offering without eligible policy.
- Incoming content remains privately staged until full SHA-256 and publication; receiving queue entries are not grants to publish. Feature owner determines accepted destination and materialization; generic journal remains authoritative for safe restart. Completed receiver replay must not fabricate additional completed items or credits.
- Keep app history separate from checkpoint retention (`protocol.md`: default 30-day **terminal journal** retention setting). Active/resumable items and accounting/settlement evidence are not expired by that setting. Queue/history retention requires an independent Settings choice when implemented, with conservative default policy and no automatic deletion of file content.

## 4. Priorities and fairness

Traffic classes and user ordering are separate:
1. **Protocol/control and interactive chat** always outrank file payload on the socket. Queue reprioritization never bypasses authenticated control traffic, negotiated limits or message scheduling.
2. **Conversation class** (explicit conversation attachments and their authenticated, bounded dependent previews) takes default precedence over ordinary bulk content.
3. **Utility class** (small profile/media assets and requested metadata/catalog artifacts) has predictable access without letting an unlimited number of generated previews/catalogs starve user transfers; preview metadata inherits the *legitimate* related conversation dependency where applicable.
4. **Bulk class** (FileShare ordinary downloads/uploads and future bulk transfers) is below conversations by default.

Represent class as a trusted local enum, not as peer-provided strings. Within a class, persist user-reorderable positions with stable FIFO tie-breaking; preserve identity/contact isolation, fairness and finite local scheduling rounds. Feature owners may register bounded dependency priority inheritance for a primary/thumbnail or other manifest-related item, never unlimited cascading promotion. Prioritize interactive conversational transfers globally but **do not allow indefinite starvation**: bound consecutive high-class admissions and grant aged eligible lower-class work a turn within local policy, while never delaying protocol/chat control messages. Initial scheduler should use a simple deterministic aging/weighted round-robin mechanism; no elaborate adaptive optimizer.

**User priority:** Reorder within a class by default. An explicit per-item `High/Normal/Low` adjustment may raise or lower eligible work *within the safe local policy envelope*, including prioritizing one selected FileShare item over other bulk items. Do not let a malicious remote offer, oversized batch or unlimited generated catalog claim conversation priority. If UI offers a deliberate `Run next` action crossing default classes, require it to be an explicit local user decision, bounded in duration/uses and still subordinate to control traffic and safety limits. Priority changes are local scheduling decisions, never sent to the peer or interpreted as authorization.

**Admission:** Respect the existing shared cap of **two active transfers per contact**, app-wide finite slots, storage/read/hash/generation budgets, socket backpressure and the current fair channel scheduler. A parked sender awaiting remote acceptance need not occupy an I/O slot; it retains bounded pending state. Fairness applies among eligible contacts/identities as well as within an individual contact. Do not acquire source/writer/temporary reservations before work is eligible or before the controlling feature approves. A transfer paused for credits is not eligible merely because its priority increased.

## 5. Preemption of large transfers

**Goal:** A large FileShare transfer must not monopolize slots indefinitely when a newly eligible higher-priority conversation attachment needs one.

- First use an idle slot. Preempt **only when relevant admission/network/storage capacity is exhausted** and there is genuinely eligible higher-priority work. Prefer an active lower-priority *bulk* transfer with the same limiting resource/contact; avoid disturbing unrelated contacts or stopping a transfer that does not free the bottleneck.
- Preemption is a **cooperative, locally requested pause**, not destruction of a coroutine, socket or storage operation. The owning `TransferSession` alone decides the safe boundary, persists/validates checkpoint evidence, stops further chunk admission, sends a typed pause where appropriate, awaits outstanding work, releases its reservation and reports a paused outcome. Restart later through the normal sender-selected, mutually verified checkpoint negotiation.
- Prefer the **next** mutually confirmed checkpoint. For an oversized interval, the existing protocol's optional deliberate pause-boundary checkpoint may be used **only if already supported and independently verified by the protocol/journal**, never by inventing an unauthenticated offset or silently weakening backpressure. If safe preemption cannot complete soon, the scheduler waits for the next existing boundary; chat *control messages* remain prioritized on the connection regardless. Never cut a write/hash/commit, leave a half-published receiver, or mark the in-flight flow ACK offset as durable.
- A sender can request an orderly outbound pause; a receiver must coordinate the same durable endpoint through the existing peer pause/confirmation protocol. A receiver cannot assume it may stop a remote sender mid-window without protocol handling. If the existing protocol cannot support **local priority preemption** safely in both directions, document the narrow gap and obtain explicit owner approval before extending it; do **not** invent another transfer protocol or substitute destructive cancellation.
- Use a **minimum uninterrupted work quantum and preemption cooldown** (configurable or bounded local defaults) to prevent high/low priority oscillation and churn. Never preempt when the transfer is in final publication, terminal reconciliation, already Pausing/Paused or too close to a safe completion boundary. Preserve active authorization/revocation barriers and local full-file checks.
- Queue-visible state distinguishes `Pausing for higher-priority transfer` from an error or user pause; such a paused transfer is automatically eligible to continue after pressure clears if authorization remains valid. A user-paused transfer remains paused until user resumes. Higher priority may justify faster scheduling, not data loss or weaker security.
- A transport peer may be slow, malicious, disconnected or uncooperative: bound pause negotiation, timeouts and per-contact outstanding states; fail to a safe persisted paused/recoverable result when the protocol allows, otherwise leave current session running or handle a normal connection failure—never hang the UI. Do not disclose which other transfers/contact caused preemption.

## 6. API and events

Provide a small C++ async interface usable by any trusted feature:
```cpp
submit(TransferIntent) -> Task<Result<WorkId>>
accept(WorkId) / reject(WorkId)
pause(WorkId) / resume(WorkId) / cancel(WorkId)
setPriority(WorkId, Priority) / reorder(WorkId, neighbor_or_position)
snapshot(filter, cursor, limit) -> bounded immutable projection
subscribe(scoped_change_callback) -> RAII handle
```
Signatures are illustrative, not a requirement for new standalone library. All actions validate local permission to operate on the referenced identity/contact/feature. An internal active session handle/transfer ID is **not** passed as a security capability to QML or remote callers. Each accepted user command has a bounded operation ID/result; the UI sees Pending/Applied/Failed and authoritative state updates, not optimistic implied success.

The feature adapter must support: validate intent/offer; prepare immutable source or destination; current authorization and security preference rechecks; typed stage/commit outcome; resume binding after restart; and optional dependency/charge admission. Avoid adding dozens of generic callbacks by default—introduce only concrete required extension points, preferably typed feature controllers.

Events and snapshots expose sanitized status, direction, trusted purpose, owning feature context, contact/identity display labels permitted locally, progress (flow vs **durable verified checkpoint** distinctly), total size if known, queue position/class, reason, and bounded timestamps. UI/metrics/MCP are **read-only projections** until they issue explicit validated commands. No sensitive paths, original remote names, peer identities or filenames as metrics labels or diagnostic log output.

## 7. Persistence, recovery and cleanup

Use one private versioned SQLite work registry (or existing application DB equivalent) with short transactions, unique idempotency keys and bounded counts/history. Persist ordering and explicit user pause/cancel decisions **before** acknowledging them. Feature completion/journal recording and queue registry are not cross-database atomic: reconcile deterministically from journal + storage evidence on startup and after a failed projection. Queue never directly rewrites checkpoint state to force a desired display result.

Startup: load a bounded page of pending work on a low-priority coroutine/worker; reconstruct feature bindings and authorization; infer interrupted runtime sessions as eligible pauses only when journal permits; admit by current priority, not raw database insertion order. Unrecoverable or missing feature bindings produce a typed local attention state, not a guessed source path. Do not automatically re-offer expired/cancelled/terminal transfers.

Cleanup: release stage/storage leases, queue resources and subscriptions on each terminal path. Terminal history may remain after generic journal evidence is compacted, but it cannot be used as proof of completed content or charge settlement. Retention/clear-history actions must not delete the user's file or a live resume/credit record. Limit pending entries, active per-purpose and per-contact counts, DB size, event log volume and UI projection batches.

## 8. Implementation and acceptance

**Implementation order, only when this milestone is explicitly scheduled:**
1. Extract persistent work registration and feature-owned purpose/binding from current `Contact::fileQueue_` / File state without altering the established single-owner coroutine attempt architecture.
2. Implement app-wide eligibility/admission/fairness and basic priority order; migrate conversation and preview transfers without breaking signed message/thumbnail manifests.
3. Migrate inbound offer/decision flows; ensure accepted and nonaccepted offers share the common queue but not storage admission.
4. Add FileShare content and catalog feature adapters with existing VFS/authorization/budget/credit exemptions; keep uploads/credits gated by their own milestones.
5. Add safe, deterministic priority preemption at a verified checkpoint **only after focused tests show the current protocol can support it**; do not make nonpreemptive queue management depend on speculative wire changes.
6. Expose bounded model/actions to `transfer_queue_ui.md`, test MCP and metrics snapshots where available, preserving feature-specific views.

**Acceptance:** user reorders pending work, process restart preserves order, higher-priority conversation attachments overtake large bulk items within finite safe boundary time, eligible bulk items eventually progress, two-per-contact/global quotas hold, paused/resumed SHA-256 integrity and checkpoint invariants survive, independent receiving/sending priorities and policy rechecks work, malicious offers cannot promote or allocate unbounded work, manifest thumbnails still accompany primaries, cancellation/revocation wins over priority, crash/restart does not duplicate transfers/credits, and the UI remains responsive under large backlogs. Automated tests must simulate deterministic checkpoint/event boundaries and slow/uncooperative peers; use the existing private-Tor MCP harness for the integration gate.

**No-goals now:** defining future social-media features, upload/credit accounting internals, full directory-browser UI, rewriting the generic transfer protocol, arbitrary host filesystem copying, realtime A/V scheduling, cross-device queue synchronization, or a second transfer worker process.
