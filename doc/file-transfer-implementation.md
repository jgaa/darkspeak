# File transfer implementation worklist

This tracks implementation of the current specifications; it does not change their contracts.
Reference baseline: `b2205ca`. Checkpoints must remain unadvertised until the generic wire/recovery gate below passes.

## Implementation status

The full feature remains incomplete. Production checkpoint advertisement stays disabled
(`Peer::localCheckpointPreference()` returns zero). The sections below remain the
acceptance worklist; the status here records implemented behavior and open gates.

Implemented generic lifecycle: versioned application checkpoint mode/interval,
typed persistent pause/failure mirrors, separate pause/credit-wait states, UI pause
explanations and incoming resume eligibility, cancellation versus partial retention,
restart recovery and authenticated identity/contact/direction lookup. Generic journal
state is authoritative when the application mirror fails. Unversioned readiness, chunks
and ACKs are rejected for every transfer.

Initial checkpoint wire transfers work behind the disabled production gate. Bilateral
capabilities freeze bounded preferences; standalone and signed attachment offers carry
independent attempt parameters outside signed content identity. Sender journal/attempt
persistence precedes the offer. Receiver storage and prefix verification precede exact
readiness. Chunks, flow ACKs, checkpoint phases and pauses bind the authenticated scope
and fresh attempt. Ordinary ACKs cannot release the checkpoint window. Advisory pause
references cannot advance progress or establish source corruption.

Nonterminal checkpoint resume now uses bounded retained candidates and correlated
request/results. The source selects its own accepted checkpoint, independently hashes
its actual prefix before ACCEPTED and persists a fresh attempt. The receiver verifies
its durable prefix before suffix invalidation, commits the selected attempt, then sends
exact readiness. The source commits mutual confirmation before admitting chunks.
Receiver prefix damage can negotiate earlier retained checkpoints in at most three
rounds under the original deadline. Receiver initiation preserves the transfer identity;
outgoing checkpoint files do not expose an independent resume action.

Initial preparation, validation and readiness share a contact-wide two-operation budget
and a 30-second monotonic deadline. Retired asynchronous operations keep their slot until
they actually return, including across connection teardown. Repeated exact readiness
is coalesced while its first journal commit is pending. The transfer scheduler retains
hashing/publication slots and uses a bounded snapshot when callbacks remove transfers.

Source absence and conservative backend metadata refusals persist distinct resumable
pauses. Only a locally computed mismatch against trusted source content establishes
`SourceIntegrityMismatch`. A peer SOURCE_CHANGED result records `RemoteSourceChanged`
on the receiver without emitting local integrity/refund proof. Known wire errors for
receiver checkpoint mismatch and unsupported transfer version now preserve their typed
terminal outcomes, instead of masquerading as user cancellation. Unknown errors still
fail closed. Active terminal assertions stop payload immediately, with the durable typed
failure replacing the terminal application mirror after journal commit.

After a rewind, the outgoing pump also compares its rebuilt byte stream with retained
accepted source-prefix hashes before sending those boundaries. Short retained pause
prefixes split hashing inside a chunk, preserving the ordinary wire chunk shape. A
conservative reader/preparation error alone still cannot establish refundable proof.

The generic progress journal is now format **3**, independently of unchanged checkpoint
wire version **1**. Keys include local identity, remote contact, transfer ID and direction.
`confirmed_offset` is the current stream fence and may rewind; `verified_prefix` is the
monotonic highest mutually verified prefix for later accounting reconciliation. It is
not a digest or resume authorization. Retention cannot lower it. Format fixtures cover
key bytes and schema. Only proved-empty version-one/two journals are transactionally
reinitialized; populated experimental journals remain untouched and unsupported. The
storage allocation identity/path remains stable. No real profile has been modified.

Regression coverage includes `transfer_checkpoints`, `protocol_v3`,
`managed_storage`, `database_recovery` and the new `checkpoint_restart` CTest
registration. Authenticated loopback cases cover all three interval sizes, empty
completion, signed primary/preview offers, withheld checkpoints, stale controls,
lost confirmation recovery, receiver-prefix fallback and distinct source failures.
The restart test runs twenty-four scenarios in fresh OS processes with isolated profiles:
unaccepted and offline-approved offers, partial progress, lost EOF receipts,
publication before/after the native commit, and preview completion with valid or
forged initial readiness, plus lower connection preferences during standalone and
preview EOF recovery, withdrawn preview permission after restart, and revocation
during a live multi-chunk preview, four non-capable reconnect cases, retry after a
readiness queue failure, a completion hint during publication after reconnect,
lost standalone/preview offers, stale initial readiness, acceptance during source
preparation, offer queue failure and a pre-offer preparation crash.
Each child has a 30-second watchdog; CTest is bounded to
180 seconds. `tests/run-all-tests.sh` discovers the new registration and was reviewed.
Only test targets and dependencies are built; no full application build is performed.

Initial readiness can restore an evicted outgoing file's runtime attempt only after
protocol-layer validation against its own persisted initial sender journal. The
candidate nonce is never installed before validation. Work uses the existing contact
budget and deadline; conflicting candidates and incorrect nonces fail before
application dispatch or payload. Outgoing signed attachments require an unambiguous
local outgoing record in the authenticated scope; missing records cannot silently
omit checkpoint metadata. Offline acceptance persists as Queued, and a new
connection negotiates a fresh zero-prefix attempt while preserving explicit approval.

Owner decision: completed-receiver EOF recovery is automatic. Matching authenticated
duplicate offers trigger bounded verification and completion replay without receiver
action. Reconnect discovery includes standalone offers and signed preview manifests;
repeated manifests retain signed identity and leave unaccepted originals untouched.
The repeated nonce is a hint; a fresh negotiated attempt controls the EOF handshake.
Changed connection preferences apply to new transfers; repeated offers retain a
larger frozen interval only when the receiver matches its own scoped immutable record.
Unknown IDs and changed content above the current preference fail before dispatch.
Both endpoints verify the selected prefix. The receiver stays Completed, reads its
published object, and never allocates another pending writer or republishes.

Publication recovery also handles a crash after native commit but before application
completion: a retained mutual final checkpoint and independently verified published
bytes permit forward recovery of the generic certificate and application reference.
A final checkpoint on an unpublished partial does not establish publication and remains
subject to explicit resume. A durable outgoing completion certificate repairs the
application mirror without reopening its source.

Receiver checkpoint admission now shares a scoped policy check across explicit
acceptance, resume, fallback, completion replay and publication reconciliation.
Thumbnail authorization rechecks the original media type and its current preview
preference separately from JPEG/file permission. Verification callbacks recheck
this policy after awaits. Active incoming checkpoint channels observe local
policy/contact/primary-state changes, stop admission and queued writes synchronously,
and persist AuthorizationWithdrawn without acknowledging new progress. Observers
are disconnected when their channel is destroyed; primary objects stay alive for
the channel lifetime. Already completed content remains published. No wire/schema
change or new MCP tool is needed; existing preference controls and resume/state
reporting use the same domain checks. Publication races remain part of the audit.

Owner decision: protocol-level backwards compatibility is not a goal. Every file
transfer requires checkpoint support, including fresh ordinary attachments. A peer
without it gets a durable UnsupportedVersion outcome before offering or admitting
file data; existing partials and completed content are not replaced. Incoming
unversioned offers, readiness, chunks, ACKs and resume are rejected before application
dispatch. The old raw send APIs fail locally and no application resume path reopens
an unversioned stream. Resume eligibility requires an incoming checkpoint transfer.
Readiness and validation-budget fixtures now use checkpoint negotiation. The four
non-capable process cases also verify that a fresh attachment aborts without an
offer or payload, replacing the previous legacy-continuation expectation.

Preparation reservations now hand off to channels without releasing their contact
slot. Each independently suspended channel operation retains a copy; publication,
failure, pause and cleanup retain the same token through their own returns. File
objects keep only a weak reference, so retained history/terminal channels cannot
hold idle slots. Source preparation/prefix rebuilding keeps its original 30-second
deadline; active payload begins afterward and uses the existing flow/checkpoint
progress timeouts. Completed EOF replay retains its bounded recovery deadline.
Disconnect cancels admission without releasing retired I/O reservations. Regression
cases suspend actual preparation/read coroutines, retire their channel, and check
that a third operation stays blocked until return, with no bytes or false corruption
proof. Native publication separately verifies this lifetime through completion.
Readiness request ownership is released on every preparation exit, including send
failure, so a cancelled or completed actor cannot leave an immortal request lease.
A real restart case injects one readiness send failure and completes on manual retry
from the retained prefix. Receiver preparation retains its Contact across awaits.

Terminal failure persistence can bind the expected journal attempt and cancellation
token under the transaction. File-domain failures use their own runtime/durable
attempt and reject stale callbacks before updating the app mirror. The only initial
offer-hint exception for typed admission failure requires an incoming Offered/Queued
file, no storage/pending object, an initial zero reference and its own unstarted
journal. Explicit user cancellation binds the current durable record, including
preparation before readiness, without promoting a sender hint into an attempt.
Known remote failures retain the existing synchronous cancellation mirror while
their typed journal outcome commits. None can fail a replacement attempt. These are internal API changes with unchanged wire and
journal versions. Existing MCP accept/resume/state reporting uses the same domain
services; no new MCP tool or schema is needed.

Completion hints received during Hashing now wait in the same two bounded retry
records until publication and retired I/O return. Duplicates coalesce without
extending the original 30-second deadline, including reconciliation and EOF replay.
Retries recheck connection scope and receive policy. Publication retains the I/O
reservation but has a separate cancellation token from its retired transport;
transport timeout/pause cannot close its writer after Hashing starts. A real
loopback reconnect regression failed before these fixes and now completes the
sender automatically without payload or a replacement published object.

Reconnect discovery also retries untouched initial offers, including lost signed
preview manifests, an offer queue failure and a crash after attempt persistence
but before emission. Only a journal containing the implicit zero checkpoint, no
payload/verified progress and no failure or pause other than Disconnected qualifies.
The same two-candidate discovery and contact work budget applies. Source checks
precede fresh persisted nonces; explicit acceptance takes priority and waits for
retired preparation using the same reservation/deadline. Startup preserves zero-byte
outgoing checkpoint offers as Offered. Receiver approval stays explicit and partials
are never automatically streamed. Initial readiness must correspond to a bounded
offer registry on the current Peer, including after file-cache eviction; stale
connection readiness cannot restore a runtime attempt. Registry decisions and
FileManager lifecycle events release entries even after the original File object
has been evicted. Registry records bind both the wire identity and local row ID;
state/deletion events for a distinct incoming row cannot release an outgoing slot.
One state observer and one deletion observer per Peer replace per-object observers,
without retaining Files or loading journals during cleanup. Successful row deletion
now emits the existing FileManager deletion signal, including for the message model.
A full 64-entry registry regression failed for cancellation, failure and deletion
before the fix and passes after it. Wire/journal versions stay unchanged.
Existing MCP help reflects the retry behavior; no new API is introduced.

Remaining generic gate: full typed error/timeout and cancellation cleanup (including
native publication races), and bounded local interval settings. Terminal mirror
recovery still changes only Paused to Failed when the journal contains a failure;
Offered or Cancelled mirrors can therefore retain the wrong terminal state after
a crash. Correcting that path, durable-failure notifications and publication-time
cancellation still require further audit.
Capability advertisement stays zero until these checks pass.

Terminal journal retention/compaction also remains open. Owner input is pending on
whether compact terminal records live with transfer history under a hard count limit,
or expire after a configurable window (proposed initial value: 30 days). Neither
option deletes downloaded files; active recovery and accounting/refund evidence must
remain protected. No expiry policy has been implemented while this choice is pending.

Storage/VFS recovery audit, persistent FileShare authorization, upload coordination,
credit ledger/admission and application integration remain in sections 3–7. Persisted
credit-wait enums do not implement credit policy. Existing MCP file decisions scope
by direction and allow paused outgoing cancellation. Automatic repair uses existing
domain services and state reporting; no new MCP tool is needed or enabled. Existing
MCP support passed a syntax check; its integration target is unavailable in the
current MCP-OFF test configuration.

The format-three journal distinguishes explicit sender acceptance of the empty final
checkpoint from the implicit initial zero prefix. Reopen regression coverage ensures
that lost receipts retain this evidence without inventing mutual confirmation.
Formats one/two have preserved fixtures and populated older catalogs fail closed;
only proved-empty catalogs can be reinitialized transactionally. The wire stays version
one. No new MCP API is needed: completion repair uses ordinary domain services and
existing transfer-state reporting; manual MCP resume eligibility remains unchanged.

Socket teardown investigation reproduced premature destruction deterministically:
a disconnect observer can release the last Peer, whose socket then dies inside
Qt's disconnected signal. All socket creation now uses a factory with a private
constructor and a shared-pointer deleter that calls `deleteLater()`. This preserves
the socket through the network callback; retry replacement also detaches old
socket-to-client callbacks before releasing its owner. Protocol regressions check
the signal lifetime and actual graceful/abrupt loopback disconnects. The lifetime
assertion failed before the fix and passed afterward. Existing Peer/socket destructor
callback guards remain in place. This ownership fix changes no wire/schema or MCP API.

Current validation: all five complete targeted suites pass with mandatory
checkpoints, retained channel/retired-I/O reservations, native publication lifetime,
readiness-failure cleanup, completion hints during publication, lost initial
offers and registry reclamation after File cache eviction: `transfer_checkpoints`,
`protocol_v3`, `managed_storage`, `checkpoint_restart` and `database_recovery`
(176.31 seconds total).
The restart suite covers twenty-four isolated scenarios. Protocol regressions reject
six forms of unversioned file traffic before dispatch. Retired preparation/read
reservations failed their regression before the fix and pass afterward. Only test
targets and dependencies were built; no full application build. Checkpoint
advertisement stays disabled until the remaining gate passes; with mandatory
checkpoints this currently disables production file transfers. Existing MCP help
states this requirement; no new MCP tool/API was added. The current test
configuration has MCP integration disabled.

## 1. Persistent generic transfer lifecycle

- Preserve existing numeric states; add distinct paused, waiting-for-credit and insufficient-credit states, machine-readable reasons and resume eligibility.
- Persist negotiation mode and interval independently of live channel attempts. Restart must not downgrade a checkpoint transfer to legacy or equate interruption with corruption.
- Scope all network transfer lookups to authenticated identity, contact and direction. An identifier collision with another contact must neither authorize access nor prevent the correct scoped lookup.
- Persist typed pauses before reporting them; old attempts cannot pause new ones. Cancellation remains terminal and removes partial content, whereas pauses retain it.
- Keep QML, history, notifications and existing MCP metadata consistent. Generic attachments do not consult FileShare credit policy.

Acceptance: restart and stale-attempt tests, colliding transfer IDs across contacts/identities/directions, pause/cancel distinction, unchanged attachment acceptance.

## 2. Generic checkpoint wire and recovery gate

- Negotiate bilateral `file-checkpoint-v1`, bounded local preferences and immutable per-transfer interval. Carry parameters through standalone and signed attachment offers without changing signed content identity.
- Bound asynchronous initial/readiness/resume work to two per contact with 30-second monotonic deadlines; recheck connection, authorization and state after every await.
- Persist initial progress and sender attempt before offering. Receiver opens storage and verifies selected prefix before ACCEPTED; sender validates the exact readiness echo before admitting chunks.
- Dispatch checkpoint/pause controls and attempt-bound chunks/ACKs only after negotiation, shape, scope, direction and nonce validation.
- Resume reports bounded retained durable candidates; sender selects only its own accepted checkpoint and revalidates actual source bytes. Receiver verifies its actual durable prefix, invalidates extra suffix, persists the fresh attempt and signals readiness.
- Recover lost confirmations, duplicate offers and completed EOF-only replay without resetting transfer identity or charged ranges. Completed content is independently reverified; it cannot restart an earlier prefix.
- Persist source unavailability/revalidation separately from locally proved source-integrity mismatch; peer digest errors never cause an automatic refund.
- Bound terminal progress retention/compaction without evicting active recovery or accounting evidence.
- Enable capability advertisement only after end-to-end negotiation, interruption, restart and terminal replay tests pass.

Acceptance: paired authenticated wire tests, 64 KiB/1 MiB/16 MiB intervals, withheld checkpoints despite flow ACKs, empty/final completion, prefix mutation, unavailable source, malformed/stale controls, concurrent resume and dropped final controls. Reject unsupported/unversioned file traffic without a legacy fallback.

## 3. Storage and VFS recovery audit

- Reconcile cross-catalog reservation intents in bounded batches; distinguish live allocations from orphan reservations before release. Do not delete unknown files.
- Retain separate permanent/incoming/temporary/cache limits under one profile's physical pool. Different profiles sharing read-only sources remain independent, with no shared journal, coordination or inter-profile lock.
- Ensure mount revocation, exclusions, catalog leases and publication recovery preserve privacy and authorizations across restart.

Acceptance: crash injection at intent/allocation/publication/cleanup boundaries, lowered quotas, independent processes, hard-link/mount/symlink aliases, hidden generations and export revocation.

## 4. Persistent FileShare authorization

- Add versioned identity-scoped group membership and rule/policy storage, bounded async loading and atomic generations.
- Use the existing policy evaluator for server enforcement and View as Contact; persist rule IDs and preserve complete contribution provenance.
- Merge quota/exchange fields independently with specified precedence; keep hard assignments, exclusions and physical limits non-overridable. EXPORT remains independent of LIST.

Acceptance: restart, complementary/conflicting groups, path DENIED/reopening, field merges, assignment confinement and active-handle revocation.

## 5. Upload recovery coordinator

- Add a private versioned SQLite intent/step journal with stable workflow IDs and idempotent participant outcomes.
- Atomically claim verified duplicate content within the specified scope. Keep manual acceptance in private quota-accounted quarantine.
- Treat FileManager publication as authoritative; after publication recover forward, keeping content hidden until acceptance registration completes. Credit approval remains separate.

Acceptance: duplicate races, every cross-database crash boundary, manual rejection/acceptance, restart reconciliation and bounded quarantine expiration.

## 6. Byte credit ledger and admission

- Persist immutable entitlements, policy-specific fractional remainders, manual adjustments, settled balance B and reservations R. Spendable balance is B-R; a transfer includes its own reservation when reevaluated. Negative B is legal; earned caps apply to B.
- Advance fractional arithmetic only on first approval. Preserve the remainder through cap clipping; revoke/reapprove the original awarded whole amount without recalculation.
- Settle uniquely confirmed prefix bytes per charge identity through the coordinator. Recover idempotently from generic progress events without a transport dependency on accounting.
- Serialize admission and rebudgeting, halt further windows on credit deficit, and persist WaitingForCredit/PausedInsufficientCredit with shortfall. Default to manual resume.
- Refund all associated debits once on locally proved source corruption. Remote assertions or incorrect receiver digests cannot trigger refunds.
- Trusted local feature purpose makes attachments, previews, thumbnails and catalogs exempt in both directions.
- Bound receipts/checkpoints/audit retention while preserving balances, entitlements, deduplication and refund evidence.

Acceptance: fractional/cap/revoke/reapprove cases, concurrent reservations, unique-prefix retransmission, crash reconciliation, negative balances, insufficient-credit pauses and exemptions.

## 7. Application integration and release checks

- Connect authorized FileShare operations, upload review, credit administration, explanations and transfer reasons to existing application models and UI. Preserve Home notifications for filesystem conflicts.
- Review existing MCP schemas/help/tests for enum and model changes; new tools require separate owner approval.
- Maintain CTest, bounded timeouts, `tests/run-all-tests.sh` and `tests/README.md`. Run targeted extended tests during development; no full application build without owner instruction.
- Record completed work and remaining gaps honestly. Future shared catalog caches, clever scheduling, tags/ratings and browser features not specified for initial implementation do not block the generic transfer gate.
