# DarkSpeak — Transfer Queue UI

**Status:** Implementation specification for the user-facing application-wide transfer queue. **Behavior/ownership:** `transfer_queue.md` is authoritative; current attempts remain owned by the generic coroutine transfer engine. This UI is **not** the FileShare browser or a replacement for attachment cards inside conversations. UI is built from C++ models, with QML presenting and dispatching simple actions.

## 1. Access and overall layout

Provide a first-class **Transfers** view accessible from the main navigation, with a compact status indicator available without opening it. The view covers **both sending and receiving** across identities, contacts and feature purposes (conversation attachments, profile media, previews, directory catalogs, FileShare content and future feature adapters). Display only authenticated locally authorized information; no UI action grants a peer any access.

Use a responsive, virtualized list/tree that remains usable at **1366×768**, with the application's scalable text settings (20–200%), keyboard navigation, light/dark themes and accessibility labels. No full-list rebuild on every progress event. Apply consistent platform-independent terms, e.g., `Waiting`, `Preparing`, `Transferring`, `Pausing`, `Paused`, `Needs approval`, `Completed`, `Failed`. Source/destination sensitive physical paths are not displayed by default.

## 2. Rows, grouping and information

Provide tabs or equivalent filters: **Active**, **Queued**, **Paused / Needs attention**, **Completed**; an **All** filter may combine them. Include per-direction filters **Sending / Receiving**, per-identity/contact filtering, and optional purpose filtering. A compact count and progress overview should distinguish active transfers from waiting offers.

Each row shows: sanitized friendly filename/title, direction, trusted feature source (Conversation, Preview, Profile, FileShare, Catalog, etc.), contact and local identity, state/typed reason, size if known, transferred bytes/progress, and whether progress is **durably checkpointed** versus merely sent/received. Show last known speed/ETA **only when meaningful**, never fabricate a reliable estimate for paused/zero/unknown totals. Rows may link back to the originating conversation, profile or FileShare browser context.

For multi-attachment messages or future content batches, optionally group under a collapsible parent; children retain independent status/progress/acceptance/cancellation. Do not let the UI flatten unrelated transfers into a single implicit permission decision. Previews should appear as clearly identified dependent transfers, not arbitrary duplicate primary files. A directory catalog is labeled **Directory listing**, not a user document.

For a currently active item show a bounded, non-blocking progress update; throttle/debounce UI notifications rather than emitting per chunk. A queue-position label may show approximate effective place and `Waiting for contact / approval / storage / credit / capacity` reason. The order displayed in Queued is the actual current scheduling order, with a clear indication when class precedence differs from user reorder.

## 3. User controls

**Queue priority/reorder**
- Pending transfers can be reordered via **Move up / Move down** and keyboard-accessible equivalents; drag-and-drop is optional but recommended if it doesn't introduce complex QML state. Include a simple priority selector **High / Normal / Low**. The user should see immediate feedback followed by authoritative confirmation or an error, with persistent ordering after application restart.
- Default priority puts conversation attachments before FileShare bulk. Within a class, manual ordering is applied. An explicit local **Run next** action may temporarily promote one selected eligible bulk item without bypassing control traffic, authorization, storage limits or starvation safeguards. Present class-based restrictions clearly rather than silently ignoring reorder attempts.
- **Receiving transfers** are present and manageable. User reordering is effective for locally controlled receive admissions, acceptance and permissible pause/resume requests. If a remote sender has already started, the UI must not falsely promise unilateral immediate control of its sending rate; show `Pausing…` until a safe peer-coordinated checkpoint boundary.
- Batch controls (`Move group`, `Pause group`, `Resume group`, `Cancel group`) apply only to explicitly grouped local work and execute as bounded independent commands, reporting partial failures rather than misrepresenting an atomic operation.

**Lifecycle operations**
- Queued: `Pause` (manual hold), `Cancel`, reorder/priority.
- Active: `Pause` (cooperative at a verified checkpoint), `Cancel` (explicit irreversible intent with confirmation when needed). While pausing show state and do not release the row/slot until the actual owner finishes cleanup.
- User-paused: `Resume`, `Cancel`, priority/reorder. Auto-preempted: show `Paused for higher-priority work` and permit manual resume where policy allows; never override a user's persistent pause.
- Incoming pending offer: `Accept` / `Reject`, with the current feature's policy and security context. No automatic action when merely opening the Transfers view.
- Failed: show safe typed reason and `Retry` **only if** journal + feature policy permit it; terminal corruption failures must not display an operative retry button.
- Completed: `Open originating context`, optionally `Save/Export` through the existing safe materialization workflow. `Remove from history` is distinct from deletion of the saved file and from purging durable checkpoint/credit evidence.

Do not enable controls that cannot be safely performed. Commands are asynchronous; show `Applying…` and server-confirmed result. For privacy and usability, avoid modal confirmation for harmless priority adjustments; confirm destructive group cancellation where it matters.

## 4. Preemption and priority feedback

When an eligible high-priority conversation attachment arrives while large bulk transfers consume the relevant slot(s), the queue may request **checkpoint-safe preemption** per `transfer_queue.md`. In the UI:
- Bulk item: `Pausing for higher-priority transfer` → `Paused for priority` with durable resumed-byte indication. Show no scary failure warning.
- Higher-priority item: `Waiting for safe checkpoint` until a slot is actually released. **Never** claim that a request will start immediately if a receiver/writer is in a noninterruptible stage or the other peer has not confirmed a safe pause.
- Avoid progress reset or duplicate history when a preempted item resumes. Show paused time separately from active-transfer speed/ETA calculations.
- Avoid thrashing animations and progress noise on repeated priority changes. Chat/control traffic stays interactive even when no transfer slot is immediately free.

An advanced, compact optional explanation may show why a transfer remains queued (policy, contact offline, two-transfer limit, credit deficit, blocked resource, higher priority, current checkpoint negotiation), but never reveal another contact's private data to a remote peer. The local owner may see their own queue across identities with privacy controls.

## 5. Models and UI contract

Expose one C++ `TransferQueueModel` (or equivalent), with bounded filtered paging/virtualization, stable work IDs, row insert/move/status update signals and typed command results. **All** sort/filter/priority/dependency calculations and state merging belong in C++; QML only renders and forwards commands. Preserve the UI architecture rules in `agents.md` (lazy-load substantial pages, retain opened lightweight views, avoid heavy work in QML). Use the application's existing appearance system rather than introducing a separate style/theme.

Suggested model roles: work ID, identity/contact display, direction, trusted purpose, grouping parent, title, state/reason, priority class and user override, queue position, bytes sent/received, durable verified bytes, total bytes, effective admission wait, source context link, available actions and last update. Never expose raw feature authorization tokens, native paths, peer-controlled internal IDs, credit records, FileManager handles or checkpoint signing material through QML.

The feature's own UI remains the familiar place for per-item actions; both the feature card and Transfers view dispatch through the *same queue command API*. A change in one view appears in the other. MCP read/control tools require their separate explicit capabilities and asynchronous operator approval for mutable operations under `mcp_and_testing.md`; test-only MCP may inspect controlled queue status in isolated fixtures.

## 6. Settings

Under Settings → Transfers, expose policy choices from `transfer_queue.md`, including:
- default class behavior **Conversation attachments first** (recommended default, preserving safety and fairness even if adjusted);
- safe automatic priority preemption of lower-priority bulk transfers (default **enabled only once checkpoint-safe support is demonstrated**; until then disabled/unavailable, not a broken control);
- optional per-item manual priority and future admission/concurrency settings bounded by protocol hard caps;
- **Terminal checkpoint journal retention** in days, default **30**, range **1–3650**, as already specified in `protocol.md` (separate from transfer history/file deletion).
Avoid making pending queue capacity, unbounded retries or abuse limits user-disableable. Scope settings to the local profile, apply changes atomically, and show whether changing a setting affects only future admission or eligible current work. Changes to retention are asynchronous and never prune active/recoverable transfers.

## 7. Acceptance and delivery

Build after core queue interfaces are stable. Test against both actual transfer sessions and deterministic fake work:
- Rendering/grouping and reliable filtering of queued/active/paused/complete inbound + outbound entries from conversations, previews and FileShare.
- Priority selector, move up/down, optional drag reorder, restart-persistent order, class restrictions and explicit local Run next.
- Conversation attachment overtakes bulk at **safe** checkpoint without losing verified progress; a slow/uncooperative peer yields waiting/timeout state without freezing UI. Auto-preempted bulk resumes fairly; voluntary user pause stays paused.
- Incoming offer approval flow; paused/failed/cancelled operation controls and typed error status; revocation changes command availability promptly.
- UI never claims checkpoint bytes equal provisional flow bytes; progress throttles under busy networks/large lists.
- Correct QML focus, keyboard controls, text scaling, theme colors, screen-reader labels and 1366×768 layout; no expensive UI-thread filesystem/hash work.
- Existing conversation attachment/preview cards and media functionality continue to work, and C++/MCP/QML acceptance tests are registered with bounded timeouts in the established runner.

**Out of scope:** full remote file browser, settings wizard, rich transfer analytics dashboard, credit-ledger calculations, tags/ratings, speculative social-media UI and an independent QML-side scheduling engine.
