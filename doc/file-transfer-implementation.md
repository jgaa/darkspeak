# File transfer implementation worklist

This is a **remaining-work checklist**, not a development diary or a new feature specification. Follow `agents.md` for explicit owner approval before adding, reopening or materially expanding tasks. Check items off only on evidence. Do not restore the superseded checkpoint branch's phase-by-phase checklist; its history is in Git (`b2205ca`, `90dc949`, `3b5f825`). The old tests are evidence of required *behavior*, not a mandate to preserve the failed ownership implementation.

## ACTIVE: coroutine-owned generic transfer refactor

**Read first:** [`doc/file-transfer-coroutine-refactor.md`](file-transfer-coroutine-refactor.md). Owner has approved replacing distributed attempt ownership, not broadening protocol scope. No legacy clients or compatibility fallback. The code currently disables production checkpoints (`Peer::localCheckpointPreference() == 0`); production transfer readiness is the acceptance gate.

- [ ] Map active transfer-state writers, event entry points and resource owners; present a concise new single-owner session boundary and migration sequence before major code changes.
- [ ] Implement one coroutine-owned session per active attempt, with awaited/joined subordinate work, bounded wire/command event delivery and one writer of execution outcomes. Preserve transport, hashing, storage and journal helpers where appropriate.
- [ ] Move **incoming** preparation, receiving, checkpoints, full-file validation, publication, cancellation and termination under that owner; keep synchronous authorization/publication safeguards.
- [ ] Move **outgoing** offer/accept/resume, bounded sending, checkpoint confirmation, source validation, completion and termination under that owner.
- [ ] Eliminate displaced competing callbacks/coroutine controllers, polling, reentrancy flags and scattered reservation-handoff logic; preserve required validation and typed errors. Do not maintain parallel old/new engines.
- [ ] Demonstrate existing wire/security/restart guarantees through deterministic targeted tests, including checkpoint resume, cancellation vs publication, lost confirmations, failure/revocation and bounded resource lifetimes. Update CTest runner registrations when changing tests.
- [ ] Enable production mandatory checkpoints and verify an actual two-instance attachment transfer and restart/resume; report completed test evidence and stop. Follow existing build restrictions; coordinate full application build with owner.

**Pending owner decision, separate from this architectural refactor:** generic terminal-journal expiry/compaction (previous proposal: 30 days). Do not implement retention policy without approval. Preserve records needed by active recovery in the meantime; future FileShare accounting evidence belongs to the exchange milestone. This decision must not justify recreating autonomous recovery machinery or delaying a working production path without a demonstrated correctness blocker.

## NEXT MILESTONES — NOT ACTIVE

Order: **FileShare normal administration UI** (`specs/fileshare_ui.md`, including its storage/VFS/group/permission foundations) → **application metrics** (`specs/metrics.md`) → **remaining FTP queues, server, browser, uploads, credits** (specification discussions to continue) → **first-class dashboard last**. Wizard later. Sections 3–7 below are deferred requirements, **not** tasks to complete during the generic refactor. Do not implement unspecified features just to tick boxes.

## 3. Storage and VFS recovery audit — DEFERRED (UI prerequisites / later FTP)

**Route carefully:** reservation accounting and basic storage recovery are prerequisites for managing storage in the normal administration UI. Mount revocation and assignment isolation are prerequisites for the mount/permission editor; **remote catalog lease/export/revocation scenarios are deferred to the FTP server/browser specification**. Do not treat all the broad acceptance examples below as generic transfer-release blockers.

- [ ] Reconcile cross-catalog reservation intents in bounded batches; distinguish live allocations from orphan reservations before release. Do not delete unknown files.
- [ ] Retain separate permanent/incoming/temporary/cache limits under one profile's physical pool. Different profiles sharing read-only sources remain independent, with no shared journal, coordination or inter-profile lock.
- [ ] Ensure mount revocation, exclusions, catalog leases and publication recovery preserve privacy and authorizations across restart.

Acceptance: crash injection at intent/allocation/publication/cleanup boundaries, lowered quotas, independent processes, hard-link/mount/symlink aliases, hidden generations and export revocation.

## 4. Persistent FileShare authorization — DEFERRED (UI foundation)

Implement persistent identity/group/contact configuration and provenance only when building `specs/fileshare_ui.md`. Existing policy code may be reused. **Remote FileShare server enforcement** is a later integration when its specification exists; do not build speculative server endpoints to satisfy this section.

- [ ] Add versioned identity-scoped group membership and rule/policy storage, bounded async loading and atomic generations.
- [ ] Use the existing policy evaluator for server enforcement and View as Contact; persist rule IDs and preserve complete contribution provenance.
- [ ] Merge quota/exchange fields independently with specified precedence; keep hard assignments, exclusions and physical limits non-overridable. EXPORT remains independent of LIST.

Acceptance: restart, complementary/conflicting groups, path DENIED/reopening, field merges, assignment confinement and active-handle revocation.

## 5. Upload recovery coordinator — DEFER until FTP uploads

Requires the FTP upload/server workflows to be specified and scheduled. This is **not** generic file transfer storage or a requirement for the normal storage/mount UI.

- [ ] Add a private versioned SQLite intent/step journal with stable workflow IDs and idempotent participant outcomes.
- [ ] Atomically claim verified duplicate content within the specified scope. Keep manual acceptance in private quota-accounted quarantine.
- [ ] Treat FileManager publication as authoritative; after publication recover forward, keeping content hidden until acceptance registration completes. Credit approval remains separate.

Acceptance: duplicate races, every cross-database crash boundary, manual rejection/acceptance, restart reconciliation and bounded quarantine expiration.

## 6. Byte credit ledger and admission — DEFER until FTP exchange

Do not implement byte-credit accounting in the generic transfer engine or block ordinary attachments on it. Credit administration controls in `fileshare_ui.md` must use the real feature service when available, or explicitly show unsupported/disabled state until the scheduled credit milestone.

- [ ] Persist immutable entitlements, policy-specific fractional remainders, manual adjustments, settled balance B and reservations R. Spendable balance is B-R; a transfer includes its own reservation when reevaluated. Negative B is legal; earned caps apply to B.
- [ ] Advance fractional arithmetic only on first approval. Preserve the remainder through cap clipping; revoke/reapprove the original awarded whole amount without recalculation.
- [ ] Settle uniquely confirmed prefix bytes per charge identity through the coordinator. Recover idempotently from generic progress events without a transport dependency on accounting.
- [ ] Serialize admission and rebudgeting, halt further windows on credit deficit, and persist WaitingForCredit/PausedInsufficientCredit with shortfall. Default to manual resume.
- [ ] Refund all associated debits once on locally proved source corruption. Remote assertions or incorrect receiver digests cannot trigger refunds.
- [ ] Trusted local feature purpose makes attachments, previews, thumbnails and catalogs exempt in both directions.
- [ ] Bound receipts/checkpoints/audit retention while preserving balances, entitlements, deduplication and refund evidence.

Acceptance: fractional/cap/revoke/reapprove cases, concurrent reservations, unique-prefix retransmission, crash reconciliation, negative balances, insufficient-credit pauses and exemptions.

## 7. Application integration and release checks — APPLY PER MILESTONE

The UI integration portion belongs to the FileShare administration UI milestone; upload/credit integrations belong to their later milestones. MCP schema review and test maintenance apply to the **specific change currently being made**, not an independent invitation to expand APIs. This section does not block the generic checkpoint gate except for its generic-transfer tests/reporting.

- [ ] Connect authorized FileShare operations, upload review, credit administration, explanations and transfer reasons to existing application models and UI. Preserve Home notifications for filesystem conflicts. **Execute per owning feature milestone** (mount/permission UI now when scheduled; upload/credit UI after their services exist), not all together.
- [ ] Review existing MCP schemas/help/tests for enum and model changes; new tools require separate owner approval.
- [ ] Maintain CTest, bounded timeouts, `tests/run-all-tests.sh` and `tests/README.md`. Run targeted extended tests during development; no full application build without owner instruction.
- [ ] Record completed work and remaining gaps honestly. Future shared catalog caches, clever scheduling, tags/ratings and browser features not specified for initial implementation do not block the generic transfer gate.
