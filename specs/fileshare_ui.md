# DarkSpeak — FileShare Administration UI

**Status:** Implementation specification (normal administration UI, phase 1).  
**Scope:** Local administrator configuration of application-wide read-only storage, identity-owned mutable storage, identity/group/contact VFS mounts, permissions and policies, group membership, upload approvals and exchange-credit administration. **Excludes** the first-run/share wizard, first-class dashboard, remote file browser, and transfer-queue view; these will have separate specifications. Do not substitute placeholders for functions in this scope.

## 1. Contracts, architecture and navigation

Authoritative behavior is defined by `file_manager.md`, `read_only_file_manager.md`, `vfs.md`, `fileshare_authorization.md`, `fileshare_architecture.md` and `mcp_and_testing.md`. This document defines **what operators can see and do**; it does not create competing storage, VFS, authorization or accounting logic. Reuse the current application's navigation, themes, layout/scaling and window-size conventions.

**C++ owns all application and UI state**, data models, selection identity, navigation context, edited/draft values, validation, calculations, sorting/filtering, authorization/provenance, async operations, error reporting and persistence. QML presents the models, forwards user events/commands, controls focus and animation, and performs only trivial visual geometry calculations. Do not implement policy or wizard logic, storage computation, command availability, business formatting, timers that poll services, or derived status in QML. C++ models must be usable and testable without constructing QML. The normal UI is authoritative; a future wizard will call the same commands.

Provide discoverable entry points in the existing identity and contact administration:
- **Application settings → Read-only storage** opens the global storage management dialog.
- **Identity → Mutable storage** opens that identity's managed storage dialog (current identity selected by default).
- **Identity → FileShare** opens the shared mount/permission editor in identity mode.
- **Identity → Groups** manages identity-scoped groups and opens the same editor in group mode.
- **Contact → FileShare** opens the same editor in contact mode; the contact editor also manages group membership.
- **FileShare administration → Upload review / Credits** provides acceptance and exchange administration for the selected identity.
These are functional navigation contracts; exact visual placement and QML component choices are implementation decisions. The future FileShare dashboard is a separate first-class main-navigation view, not embedded here. Storage configuration lives in dialogs; do not turn it into a second dashboard.

Every displayed operation has a C++ command/result and an authenticated **test-MCP** operation or equivalent reusable test harness exposing the same model/service contract; see §10. Long operations report accepted/working/completed/failed/cancelled, provide stable operation IDs when needed, and do not block the UI. Commands enforce current permissions and revisions at execution time, not merely by disabled controls. All state views distinguish loading, empty, ready, stale/updating, conflict, unavailable and failed where applicable. Show concrete operator-actionable errors, not silent failures. Persistent changes update other affected views and active permissions promptly; suppress stale completions using generations.

## 2. Global read-only storage dialog

List all **application-wide registered read-only resources**, shared across local identities but never made remotely visible merely by registration. For each row expose stable resource/name, canonical local path (local administration only), backend/type, indexing/reconciliation state and progress, hash strategy (`Cached`, `Rehash`, `Snapshot`), symlink policy, size/object counts when available, last checked, errors, identity assignments and mount-reference counts. Use bounded asynchronous listing/filtering; local paths never enter remote metadata or public metrics.

Actions:
- **Add existing directory:** browse/select local directory, name/validate, choose supported hash and symlink settings, review private data/exclusions, register and start bounded low-priority indexing. If a physical source is already registered, reject duplicate registration and identify the existing resource.
- **Inspect/edit:** display the configured source, indexing/scan details, errors, supported backend settings, exclusions and identity assignments. Allow supported safe changes, rescan/reindex, cancel a running scan and retry failures; distinguish actions that may invalidate hashes or readers.
- **Assign/unassign identity or allowed subtree:** show all local identities and current assignments; assignment only bounds availability for VFS mounts, never grants a remote contact access. Validate assignment containment and optional external regular-file permission; reauthorize/revoke affected handles on withdrawal.
- **Remove resource:** display dependent identity assignments, mounts, active operations and possible effect; refuse implicit cascading removal, first require explicit dependency resolution, then remove registration/index metadata safely without deleting source directory or files.
- **Inspect mounts:** show every identity/group/contact mount referencing the resource and navigate to the owning editor.

**No physically overlapping registered read-only roots.** Reject identical roots, ancestor/descendant roots, and aliases via canonicalization, symlinks, bind mounts or other discoverable same-object ancestry. Reject ambiguous/unverifiable overlap rather than pretending it is disjoint. A desired subset/superset requires reorganizing registered shares or physical directories, not overlapping registrations. Validate both initial registration and path edits; detect overlap introduced externally and mark implicated resources conflicted, preventing unsafe operations until resolved. This registration rule does **not** prevent mounting an already registered resource/subtree more than once at differing VFS paths where authorization permits.

The `SymlinkPolicy::Always` setting and assignment-level external-file capability must be displayed **in red** with the explicit warning from `agents.md`: it can expose individual external regular files; external directory traversal remains forbidden. Keep `Never` as default, and display `WithinVfs` semantics accurately. Excluded/private paths are never selectable as exposed roots.

## 3. Identity mutable storage dialog

An identity owns one or more managed mutable storage **devices/namespaces**, never automatically shared with other identities. Opening from an identity uses that identity; when launched globally with multiple identities, allow identity selection defaulting to the current identity. Show stable device ID/display name, backend kind/capabilities, locally configured physical location (when meaningful), usage by permanent/staging/temporary/cache classes, outstanding reservations, quotas/physical-pool health, status, default marker, assigned mounts and active dependencies.

Exactly **one designated default mutable device per existing identity**, provisioned on identity creation and recoverably restored during startup. This designation is not a VFS mount or a permission grant. Default device is used for that identity's internal temporary artifacts, thumbnails/previews and cache where the required capabilities exist; users can also mount user-visible directories from it. Private internals are excluded from VFS exposure even when an otherwise permitted parent is mounted.

Actions:
- **Create/add managed device:** select identity, backend/type (initially implemented types only), supported physical location, name, quotas/class budgets; validate ownership and physical storage-pool constraints; asynchronously create and report terminal state. Do not expose internals or automatically mount the device.
- **Inspect/edit:** display supported capability, health, capacity, pool limits, current usage/reservations, retention, errors, mounted locations and active leases/writers; edit safe mutable settings with validated impact and typed errors. Settings unsupported by a backend are accurately unavailable, not simulated.
- **Change default:** select another compatible device. Validate capability for internal temp/cache requirements and destination capacity, handle in-flight leases and durable references, migrate/rehome required private data or retain safe bounded references until completion. Switch the designation atomically/recoverably; retain the old default until the new one is operational. Cancel/failure must not produce zero or two effective defaults.
- **Remove device:** disallow removal of the currently designated default. Show dependent mounts, active transfers, artifacts and leases; require explicit resolution and safe cleanup/recovery before unregistering. **Never interpret device removal as implicit deletion of underlying user files.** Any destructive purge is a distinct explicit, correctly scoped action with confirmation and recovery requirements.
- **Open mounts:** navigate to the relevant identity/group/contact mount configurations.

Mutable device locations that overlap other managed roots or protected private directories must pass the backend's ownership/isolation validation; avoid silently conflating namespaces. Storage resource and VFS mount lifecycles are independent. Refresh after restart, identity change, failed migrations or external disk loss without blocking interaction.

## 4. Unified mount and permission editor

Implement **one reusable C++ editor model and command set** with context `(identity, scope=Identity|Group|Contact, scope_subject_id?)`. One corresponding UI works in all three modes; changing context invalidates stale selections, pending edits and async results safely. Do not clone permission resolution in three models.

The primary list shows `/virtual/path → resource_name [local_source_when_administrator] (read-only|mutable)`, source/assignment, definition owner (identity/group/contact), inherited/local/replaced/conflicted/hidden status and key effective permissions. Include virtual synthetic directories where necessary, but distinguish them from actual mounts. Offer bounded filtering/search and stable IDs, rather than binding actions to row numbers or physical paths.

**Visibility and editability by tier:**
- Identity mode: edit identity-owned mount definitions and identity path rules; no group/contact records are mutated.
- Group mode: display identity mount definitions and locally defined group additions/replacements. Inherited structural definitions are **read-only**. Local permissions and policy rules may override inherited rules; expose their own configured value and inherited/effective result. Group views are illustrative: a member's other groups and contact rules can change its actual view.
- Contact mode: display the effective composition of identity + **all applicable groups** + contact definitions. Inherited structures remain read-only; local permission/path rules are editable. Show conflicting group mounts as conflicts, not silently choose a winner. A contact-specific mount can explicitly replace inherited topology at the same path.
- At group/contact tier, an explicit **Replace inherited mount here** operation creates a new higher-tier definition, never edits the inherited record. Warn about shadowed inherited descendants and require confirmation. **Remove local override** restores lower-tier resolution, not deletion of the underlying source.

Mount definition actions: add selected identity-assigned source (possibly assigned subtree) at validated absolute VFS path, inspect/edit local path/display settings, rename/move local mount, remove only local definition, explicit replace/shadow and remove replacement, inspect source resource, and navigate to storage management. Show source backend read/write capabilities and source ownership. Validate collisions, ambiguous group overlays, parent shadowing, nested mounts, mount cycles, assigned-subtree boundaries, backend exclusions and reserved/private names before commit. Moving/removing a mount must not move/delete data; ordinary source directories with nested mounts follow `vfs.md` restrictions. The editor must not create/delete physical storage as a side effect.

Provide a **separate effective-VFS preview**, with selected identity/contact and virtual path, using the production resolver. For identity/group editing, operator can select a particular contact to obtain a definitive view; otherwise explain that displayed group effective values are hypothetical. Preview must show navigation-only ancestors, hidden/unavailable nodes and conflicts appropriately to the **local operator**, while an actual remote view must not leak hidden nodes. For an inherited row, identify the tier and owning group/contact for provenance; do not imply that a source/backend capability or assignment can be overridden by permissions.

## 5. Permissions and policy editor

The selected mount/path has a rule table covering `LIST`, `READ/DOWNLOAD`, `UPLOAD/CREATE`, `WRITE/REPLACE`, `RENAME`, `DELETE`, `RECURSE`, `EXPORT`, and the independent path-level subtree `DENIED` barrier. Each operation exposes:
- **Configured local value** `Unspecified/Inherit | Allow | Deny` (and the applicable subtree barrier).
- **Effective result** with source identity/group/contact, policy generation and full evaluated winning/**losing** provenance; never represent a hard backend/assignment limit as an overridable rule.
- An **Explain** action opening a structured trace of relevant ancestors, path specificity, merged groups, conflict decisions, assignment/backend hard caps and why this outcome applies. For multiple groups, preserve each group's contribution, with accessible text and independently identifiable visual provenance.
- Create/edit/remove **permission-only path rules**, including paths without their own mount, without changing mount topology. Clear local override returns to inherited resolution.
- **View as Contact** with real effective results; identity/group mode must not claim one group-only hypothetical result is universally true.

Follow `vfs.md` and `fileshare_authorization.md` precisely: per-operation inheritance, group `DENY > ALLOW > UNSPECIFIED`, higher-tier overrides, path-level subtree `DENIED`, separately mounted descendants despite `RECURSE` denial, and explicit `EXPORT` at catalog root plus normal visibility checks. Show an explicit distinction between listing, reading, recursion, catalog export and mutation. Configuration changes affecting connected peers are effective immediately through the same authorization engine, including in-flight reauthorization. Any potentially broadening permission edit displays the affected scope before saving; apply versioned transactional changes, with confirmation on material exposure expansion.

The same editor exposes applicable FileShare policies and quotas at each tier using an extensible field model: local value (`Inherit` distinct from explicit `Unlimited` and `Disabled`), effective value, group-merge rule, hard ceiling, reason/provenance and edit/reset operations. Include upload file acceptance policy (automatic/manual), credit approval (automatic/manual/disabled), byte-earning ratio, credit-required/unrestricted download policy, partial-credit allowance, earned-credit cap, upload/file/storage/rate/concurrency quotas and supported retention/limit settings. Only offer fields supported by current implementation; if a later-phase field is visible, mark it clearly unavailable and explain why, rather than accepting a no-op configuration. Ratio is **byte-only**, checked rational integers; file-count quotas protect resources, **never** act as exchange-credit units. Higher-priority policy and safety limits may differ from mount permissions; use the effective evaluator rather than extrapolating permission precedence.

## 6. Groups and contact membership

Groups are **identity-scoped general contact groups**, owned by the shared contacts/groups subsystem, not by FileShare. The identity-level Groups interface must list/search/create/rename/delete groups, display member counts and FileShare mount/rule dependencies, and allow opening the unified editor in group mode. The group editor shows **all members** with paginated/filterable identity-scoped contact data and add/remove membership actions. The contact editor displays **all groups for that identity**, and permits membership add/remove there. Both entry points operate on the same membership service and reflect changes immediately.

Deleting a group shows its members, group-owned mounts, permissions/policies and effects on all members' effective VFS; require confirmation and a recoverable configuration transaction. Removing membership must invalidate affected contact VFS/authorization immediately, revoke access as required and update active views. Group rename preserves stable group ID and configured policies; deletion/recreation does not. The UI must handle members belonging to multiple groups, duplicate requests, concurrent edits, stale generations and policy conflicts. Present only the current identity's eligible contacts; do not allow cross-identity membership through the editor.

## 7. Upload review and exchange-credit administration

Provide a discoverable per-identity FileShare administration interface (may be multiple dialogs within the normal administration UI), backed by existing acceptance and accounting services. This is **not** generic message attachment administration and must not mix feature accounting.

**Upload review:** list pending/accepted/rejected/failed/quarantined uploads with identity, contact, immutable upload ID, offered name and verified size/hash when available, destination mount, quota status, acceptance decision, credit approval state and actionable failure reason. Filter by state/contact without loading unbounded rows. Actions accept/reject pending file, approve/revoke/reapprove credit independently, inspect immutable captured award and its history, and navigate to source contact/mount. Never publish unreviewed content or award before acceptance; reject/duplicate handling obeys the durable upload coordinator. Explicitly differentiate file acceptance from credit approval.

**Credit accounts:** list per-identity/per-contact byte-based settled balance `B`, current reservations `R`, spendable `B-R` (negative permitted), initial/free grants, earning policy, cap, unrestricted/credit-required status, pending entitlements and paused transfers affected by credits. For selected account, view bounded ledger/entitlement and charge history, including credited/revoked state, settled progress and current reservation. Allow manual grant, deduction, **set balance** (including zero), and entitlement approval/revocation/reapproval with confirmation of signed amount and downstream impact; validate integers/overflow. Display credit reservation rebudgeting/pause reason promptly. Preserve first-approval award and fractional remainder semantics from `fileshare_authorization.md` rather than recalculate awards in the UI. Show any pending coordinator-recovery state and disable operations whose outcome would be indeterminate until reconciled.

Permissions/policy editing for identities/groups/contacts lives in §5; this section is for current operational credit and file-review actions. Preview/thumbnail/catalog transfers in **either direction** and all non-FileShare feature transfers never appear as credit debits/awards. The generic transfer manager remains app-wide and feature-neutral; this UI uses its existing feature-scoped status/command interfaces, not its own transfer engine or duplicate transfer queue.

## 8. Shared interaction, validation and failure behavior

All forms load existing values and support edit/cancel/reload. C++ retains draft state until explicit save; closing without saving discards changes after warning if dirty. Validate field syntax and semantics before save, then revalidate authoritative versions under an atomic/recoverable commit. Report conflicts with enough context to reload/retry rather than overwrite concurrent changes. A completed save reflects a committed configuration change, not merely scheduled work. Multi-record storage/mount/rule operations must not leave accidentally exposed partial configuration. Async post-commit indexing or reconciliation is visibly separate from successful registration/configuration.

Any destructive/unsharing operation shows impacted mounts, contacts or dependent objects where obtainable, and specifically distinguishes **remove mount**, **unassign source**, **unregister storage**, **delete managed content** and **revoke authorization**. Never silently cascade across these boundaries. Use stable object IDs in commands, not display paths or selection indexes. Only allow UI actions supported by the selected backend, current identity/scope and true operation state. No direct SQL or backend mutation from QML.

Use bounded background queries, pagination, cancellation and generation-aware updates. Display statuses during indexing, capacity shortages, missing disks, read-only backends, failed coordinator recovery and policy conflicts. UI remains responsive during large scans, hashing, checkpoint/reconciliation work and changes affecting many contacts. Avoid per-row expensive policy evaluation; batch/cache derived C++ summaries per generation and compute detailed provenance on demand. No UI-specific logic may weaken authentication, storage limits, mount confinement or authorization.

## 9. C++ model contract

Names may follow existing project conventions, but the externally testable capabilities are required:

| Model/service facade | Required data and commands |
| --- | --- |
| Global read-only resource model | paginated resources, detail, create/edit/remove, overlap validation, scan/status, assignment management, reverse references |
| Identity mutable storage model | identity devices, mandatory default designation, create/edit/remove, change default/migration status, capacity/classes/reservations, dependencies |
| Scoped mount configuration model | context switch, inherited/local/conflicting rows, mount CRUD/replace/remove override, source selection, configuration generations |
| Scoped permission/policy model | local tri-state/field edits, effective values, full provenance, path-only rules, View as Contact, prospective validation |
| Identity group membership model | groups CRUD, group members and contact group memberships, dependencies/invalidation |
| FileShare upload review model | pending/accepted items, approval/rejection, entitlement decisions, coordinator state |
| Credit administration model | per-account B/R/spendable, grants/deductions/set, entitlement/ledger bounded views and related paused transfers |

Models expose stable IDs, typed machine-readable status/error codes, loading/dirty/validity/current-generation properties, asynchronous operation IDs/results and bounded page/query operations. QML presents localized/human-readable text from C++-supplied or centralized presentation data; no parallel business-logic calculation. Optional implementation may share the same underlying model facade across several screens, but each interface's operations and states remain present and independently testable.

## 10. Test MCP and acceptance tests

Use the **test-only MCP server** from `mcp_and_testing.md`; it invokes the **same C++ models/services and validation** as QML, with controlled isolated test identities/storage roots. Production MCP policies and asynchronous operator approval remain unaffected; privileged test tools/fault injection are excluded from release builds. Test MCP does not gain automatic remote access to physical paths beyond explicitly controlled test storage.

Minimum MCP coverage:
1. Create/reload an identity; assert exactly one protected default mutable device. Add second device, switch default, simulate failure and recovery, deny deletion of active default, inspect quotas and dependent mounts.
2. Register read-only resource; reject same/ancestor/descendant/alias/bind-mounted physical overlap; assign it to two identities while sharing one index; ensure no remote exposure without a mount.
3. Add identity mount to assigned resource; create group and contact mounts, inspect inherited structural read-only flags, permission overrides, shadow/replacement semantics, dependency warnings and removing only local override.
4. Create groups; edit membership from both group and contact interfaces; inspect complete multi-group effective permission/provenance, group conflict, `DENIED`/RECURSE/EXPORT distinctions; confirm authorization decisions through actual VFS server path.
5. Edit identity/group/contact policies and quota fields; test inherit vs unlimited/disabled, hard caps and generation invalidation while operations run.
6. Review accepted/rejected/quarantined uploads and separate credit approval; validate one-time awards, revocation/reapproval, B/R/spendable, negative balances, manual set zero and insufficient-credit pause; verify previews/catalog/message attachments never affect FileShare credits.
7. Restart between create/update/remove and during durable operation/recovery; confirm same model state and no half-visible shares.
8. Exercise invalid paths, inaccessible directories, lost disk, full disk/quota, stale UI generations, cancellation, overlapping operations, errors and nonblocking indexing.

MCP acceptance tests should await **terminal operation IDs/events**, not arbitrary sleeps. Register C++/MCP tests with CTest and maintain `tests/run-all-tests.sh` according to `agents.md`. Add focused QML integration checks for navigation, dialogs opening, binding updates, enabled/disabled actions and full workflows: MCP alone cannot prove that a control is present or connected. A component is not complete until all its specified operations are accessible in the UI and independently testable through C++/MCP. If a backend feature belongs to a later implementation phase, the corresponding UI must display an accurate disabled/unavailable state rather than a nonfunctional apparent control.

## 11. Delivery phases and non-goals

Implement in independently testable slices: (1) global RO storage dialog and identity mutable devices; (2) shared scoped mount editor and effective permission/provenance; (3) groups/membership and policy editor; (4) upload review and credit administration, gated by implemented backend; (5) acceptance tests and QML workflow coverage throughout, not deferred to the end. Keep existing messaging and attachment UI functional at every stage.

**Deferred:** simplified setup wizard; first-class metrics/dashboard view (`fileshare_dashboard.md`, using `metrics.md`); remote file browser; queue/history view; additional backends not yet implemented; redesign of the application's themes/components. This document deliberately defines **functionality**, not QML controls, exact layout or visual mockups. Do not couple these administration views to the dashboard's existence.
