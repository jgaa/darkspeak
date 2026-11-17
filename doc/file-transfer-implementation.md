# File transfer implementation worklist

Tracks work against current specifications; **not** a new source of requirements. Baseline: `b2205ca`. Follow `agents.md`: only owner-approved tasks may be added or materially expanded. Maintain `- [x]` for verified completion, `- [ ]` for unfinished work. Record evidence briefly; Git history contains detailed change logs.

## Current milestone: finish the generic checkpoint release gate

**Production blocker:** `Peer::localCheckpointPreference()` still returns zero; checkpoint support is mandatory for every file transfer, so normal production transfers cannot run until enabled. **Protocol-level backward compatibility is a non-goal:** no legacy peers, old transfer mode, fallback, or compatibility work. Reject unsupported/unversioned file traffic safely; do not build legacy tests except tests that verify rejection.

**Remaining gate:** terminal mirror recovery, durable-failure notifications, cancellation/publication races, typed timeout/error cleanup, bounded local interval settings, terminal retention decision, and production activation/validation. The detailed, owner-approved checkbox worklist follows in §1–7; **do not append new requirements or reopen checked items without owner approval**. New findings should be reported with scope and blocker evidence, not silently added.

## Completed-work evidence (concise)

- [x] Core mandatory checkpoint wire mode, flow backpressure, attempt binding, persistent progress, sender-selected resume and mutual SHA-256 confirmation implemented behind the disabled production gate.
- [x] Distinct pause/source-failure outcomes, restart/EOF replay, signed preview offers, lost-offer retry and scoped transfer lifetime handling implemented with targeted regression coverage.
- [x] Tests `transfer_checkpoints`, `protocol_v3`, `managed_storage`, `checkpoint_restart` (24 isolated restart scenarios) and `database_recovery` reported passing on latest recorded run (176.31 s). **This is Codex's reported result, not a full production application build or independent verification.**
- [x] Production/non-test wire rejects unversioned data by policy; tests cover rejection. No legacy interoperability is required.
- [x] Socket teardown last-owner lifetime fixed with deferred deletion and loopback regressions.

Source changes and prior investigative details: commits `b2205ca`, `90dc949`, `3b5f825` and test fixtures. Keep this summary short; do not duplicate an incremental diary in this file.

## 1. Persistent generic transfer lifecycle

- [x] Preserve existing numeric states; add distinct paused, waiting-for-credit and insufficient-credit states, machine-readable reasons and resume eligibility.
- [x] Persist negotiation mode and interval independently of live channel attempts. Restart must not downgrade a checkpoint transfer to legacy or equate interruption with corruption.
- [x] Scope all network transfer lookups to authenticated identity, contact and direction. An identifier collision with another contact must neither authorize access nor prevent the correct scoped lookup.
- [x] Persist typed pauses before reporting them; old attempts cannot pause new ones. Cancellation remains terminal and removes partial content, whereas pauses retain it.
- [x] Keep QML, history, notifications and existing MCP metadata consistent. Generic attachments do not consult FileShare credit policy.

Acceptance: restart and stale-attempt tests, colliding transfer IDs across contacts/identities/directions, pause/cancel distinction, unchanged attachment acceptance.

## 2. Generic checkpoint wire and recovery gate (current milestone)

- [x] Negotiate bilateral `file-checkpoint-v1`, bounded local preferences and immutable per-transfer interval. Carry parameters through standalone and signed attachment offers without changing signed content identity.
- [x] Bound asynchronous initial/readiness/resume work to two per contact with 30-second monotonic deadlines; recheck connection, authorization and state after every await.
- [x] Persist initial progress and sender attempt before offering. Receiver opens storage and verifies selected prefix before ACCEPTED; sender validates the exact readiness echo before admitting chunks.
- [x] Dispatch checkpoint/pause controls and attempt-bound chunks/ACKs only after negotiation, shape, scope, direction and nonce validation.
- [x] Resume reports bounded retained durable candidates; sender selects only its own accepted checkpoint and revalidates actual source bytes. Receiver verifies its actual durable prefix, invalidates extra suffix, persists the fresh attempt and signals readiness.
- [x] Recover lost confirmations, duplicate offers and completed EOF-only replay without resetting transfer identity or charged ranges. Completed content is independently reverified; it cannot restart an earlier prefix.
- [x] Persist source unavailability/revalidation separately from locally proved source-integrity mismatch; peer digest errors never cause an automatic refund.
- [ ] Define and implement terminal retention/compaction without evicting active recovery or accounting evidence. **Owner decision pending:** initial proposed 30-day expiry; do not implement a policy until approved.
- [ ] Close the finite release gate: reconcile journal terminal failures to all application mirror states (including Offered/Cancelled), make durable-failure notifications reliable, test cancellation at native publication boundaries, verify typed error/timeout resource cleanup and bounded local interval configuration. Fix defects found within these approved contracts; request approval before adding new worklist items.
- [ ] Run agreed targeted tests plus one production application-level checkpoint transfer with checkpoint advertisement enabled; record results and **enable mandatory checkpoint advertisement** after the preceding checks pass. No fallback for non-checkpoint peers.

Acceptance: paired authenticated wire tests, 64 KiB/1 MiB/16 MiB intervals, withheld checkpoints despite flow ACKs, empty/final completion, prefix mutation, unavailable source, malformed/stale controls, concurrent resume and dropped final controls. Reject unsupported/unversioned file traffic without a legacy fallback.

## 3. Storage and VFS recovery audit

- [ ] Reconcile cross-catalog reservation intents in bounded batches; distinguish live allocations from orphan reservations before release. Do not delete unknown files.
- [ ] Retain separate permanent/incoming/temporary/cache limits under one profile's physical pool. Different profiles sharing read-only sources remain independent, with no shared journal, coordination or inter-profile lock.
- [ ] Ensure mount revocation, exclusions, catalog leases and publication recovery preserve privacy and authorizations across restart.

Acceptance: crash injection at intent/allocation/publication/cleanup boundaries, lowered quotas, independent processes, hard-link/mount/symlink aliases, hidden generations and export revocation.

## 4. Persistent FileShare authorization

- [ ] Add versioned identity-scoped group membership and rule/policy storage, bounded async loading and atomic generations.
- [ ] Use the existing policy evaluator for server enforcement and View as Contact; persist rule IDs and preserve complete contribution provenance.
- [ ] Merge quota/exchange fields independently with specified precedence; keep hard assignments, exclusions and physical limits non-overridable. EXPORT remains independent of LIST.

Acceptance: restart, complementary/conflicting groups, path DENIED/reopening, field merges, assignment confinement and active-handle revocation.

## 5. Upload recovery coordinator

- [ ] Add a private versioned SQLite intent/step journal with stable workflow IDs and idempotent participant outcomes.
- [ ] Atomically claim verified duplicate content within the specified scope. Keep manual acceptance in private quota-accounted quarantine.
- [ ] Treat FileManager publication as authoritative; after publication recover forward, keeping content hidden until acceptance registration completes. Credit approval remains separate.

Acceptance: duplicate races, every cross-database crash boundary, manual rejection/acceptance, restart reconciliation and bounded quarantine expiration.

## 6. Byte credit ledger and admission

- [ ] Persist immutable entitlements, policy-specific fractional remainders, manual adjustments, settled balance B and reservations R. Spendable balance is B-R; a transfer includes its own reservation when reevaluated. Negative B is legal; earned caps apply to B.
- [ ] Advance fractional arithmetic only on first approval. Preserve the remainder through cap clipping; revoke/reapprove the original awarded whole amount without recalculation.
- [ ] Settle uniquely confirmed prefix bytes per charge identity through the coordinator. Recover idempotently from generic progress events without a transport dependency on accounting.
- [ ] Serialize admission and rebudgeting, halt further windows on credit deficit, and persist WaitingForCredit/PausedInsufficientCredit with shortfall. Default to manual resume.
- [ ] Refund all associated debits once on locally proved source corruption. Remote assertions or incorrect receiver digests cannot trigger refunds.
- [ ] Trusted local feature purpose makes attachments, previews, thumbnails and catalogs exempt in both directions.
- [ ] Bound receipts/checkpoints/audit retention while preserving balances, entitlements, deduplication and refund evidence.

Acceptance: fractional/cap/revoke/reapprove cases, concurrent reservations, unique-prefix retransmission, crash reconciliation, negative balances, insufficient-credit pauses and exemptions.

## 7. Application integration and release checks

- [ ] Connect authorized FileShare operations, upload review, credit administration, explanations and transfer reasons to existing application models and UI. Preserve Home notifications for filesystem conflicts.
- [ ] Review existing MCP schemas/help/tests for enum and model changes; new tools require separate owner approval.
- [ ] Maintain CTest, bounded timeouts, `tests/run-all-tests.sh` and `tests/README.md`. Run targeted extended tests during development; no full application build without owner instruction.
- [ ] Record completed work and remaining gaps honestly. Future shared catalog caches, clever scheduling, tags/ratings and browser features not specified for initial implementation do not block the generic transfer gate.
