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
state is authoritative when the application mirror fails. Legacy readiness, chunks
and ACKs cannot activate a checkpoint transfer.

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

The generic progress journal is now format **2**, independently of unchanged checkpoint
wire version **1**. Keys include local identity, remote contact, transfer ID and direction.
`confirmed_offset` is the current stream fence and may rewind; `verified_prefix` is the
monotonic highest mutually verified prefix for later accounting reconciliation. It is
not a digest or resume authorization. Retention cannot lower it. Format fixtures cover
key bytes and schema. Only proved-empty version-one journals are transactionally
reinitialized; populated experimental journals remain untouched and unsupported. The
storage allocation identity/path remains stable. No real profile has been modified.

Regression coverage extends existing `transfer_checkpoints`, `protocol_v3` and
`managed_storage` registrations. Authenticated loopback cases cover all three interval
sizes, empty final completion, signed primary/preview offers, withheld checkpoints,
stale controls, lost confirmation recovery, receiver-prefix fallback, source absence,
source mismatch and metadata-only refusal. Additional tests cover shared work budgets,
direction collisions, journal reopen, verified-prefix retention/rewind and empty-v1
reinitialization crash rollback, and corrupt catalog metadata (including narrowing,
invalid attempts/sequences, boolean flags and unproved completion). Final verification: the complete `transfer_checkpoints`, `protocol_v3` and
`managed_storage` CTest suites passed (73.04 seconds total), including the retained
short-prefix regression. Only their targeted test binaries and dependencies were
built; no full application build was performed.

Existing MCP file decisions now select direction before choosing the transfer and allow
cancellation of paused outgoing transfers. No new MCP API/tool was added. MCP support
code passed a syntax check; its integration target is unavailable in the current MCP-OFF
test configuration. Existing resume help continues to describe the incoming domain action.
`tests/run-all-tests.sh` was reviewed; these cases extend existing registered executables
and retain bounded CTest timeouts, so the runner needs no catalog change.

Remaining generic wire/recovery gate: completed published-file EOF-only replay;
duplicate-offer/readiness recovery after restart; dropped terminal controls and real
process restart/crash boundaries; full typed error/timeout cleanup; bounded
terminal retention/compaction preserving recovery and accounting evidence; and bounded
local interval configuration before enabling advertisement. Storage/VFS recovery audit,
persistent FileShare authorization, upload coordination, credit ledger/admission and
application integration remain in sections 3–7. Persisted credit-wait enums do not
implement credit policy. No new remote FileShare tools are enabled.

Open owner question: should a duplicate offer automatically trigger bounded verification
and EOF-only replay for an already published receiver, or should that replay require an
explicit receiver action? The protocol permits replay but does not select its trigger.
Implementation of that behavior is paused pending this decision.

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

Acceptance: paired authenticated wire tests, 64 KiB/1 MiB/16 MiB intervals, withheld checkpoints despite flow ACKs, empty/final completion, prefix mutation, unavailable source, malformed/stale controls, concurrent resume and dropped final controls. Preserve legacy attachment tests while legacy remains supported.

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
