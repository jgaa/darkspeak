# DarkSpeak — MCP Integration and Automated Acceptance Testing

**Status:** Draft v1 — owner decisions incorporated
**Scope:** Production MCP interface, authorization, test extensions, automated acceptance testing.

## 1. Goals

Integrate MCP into DarkSpeak for two purposes:

1. **Production:** Allow local AI agents and other MCP clients to interact with DarkSpeak under explicit user-defined permissions and asynchronous operator approval.
2. **Testing:** Allow Codex to orchestrate independent DarkSpeak instances, inspect their behavior, investigate regressions, and develop permanent Python integration tests.

Requirements:

- Reuse the existing `yahat-cpp` MCP implementation and DarkSpeak's C++ application services.
- MCP is optional at compile time and runtime.
- Production and test MCP share infrastructure; test-only capabilities are excluded from release builds.
- All mutable production operations and sensitive reads require authorization through a central asynchronous approval manager.
- MCP operations use the same application logic as the GUI.
- Networking, cryptography, validation, persistence, and authorization remain unchanged by test instrumentation.
- Tests verify observable behavior between real application instances.
- Python executes deterministic regression tests independently of AI services.
- Security, privacy, bounded resource usage, and UI responsiveness take priority.

MCP extends DarkSpeak's existing application interfaces. The C++ services and models remain authoritative; QML presents state and requests operator decisions.

## 2. Build and Runtime Configuration

CMake options:

```cmake
option(DARKSPEAK_WITH_MCP "Enable MCP integration" OFF)
option(DARKSPEAK_WITH_TEST_MCP "Enable MCP test extensions" OFF)
```

`DARKSPEAK_WITH_TEST_MCP` requires `DARKSPEAK_WITH_MCP`.

Production builds exclude test-only tools, approval simulation, fault injection, and privileged diagnostics at compile time.

Fetch `yahat-cpp` with CMake `FetchContent` only when MCP is enabled, from
`https://github.com/jgaa/yahat-cpp.git`, initially pinned to commit
`67cb6c31c13f695fc548c71f7789a0619b1333d4`. Set `YAHAT_ENABLE_MCP=ON`,
`YAHAT_WITH_TESTS=OFF`, and `YAHAT_WITH_EXAMPLES=OFF` before making it available;
disable unneeded metrics. Link `yahat-cpp::yahat-cpp`. Follow existing external
library integration patterns. Do not assume an installed package, use a sibling
checkout as a build dependency, or automatically discover local source trees.
`../yahat-cpp` is reference material for its usage skill, documentation, and API
inspection only. Keep generated headers and library from the same pinned build.
Flatpak/offline source preparation must provide the same pinned dependency
without relying on a sibling checkout.

Runtime settings:

- MCP enabled/disabled, default disabled.
- HTTP listen address, default `127.0.0.1`; accept only literal loopback addresses.
- HTTP port, configurable.
- API-key management.
- Approval timeout, default 60 seconds.
- Persistent approval and contact-policy management.

Changes take effect when settings are saved. Validate and bind a replacement
listener before retiring the working endpoint. On failed reconfiguration, retain
the working endpoint and its effective settings and report the failure. Reuse the
listener for policy-only changes. Never expose an unauthenticated interim endpoint.

Enabling starts the MCP endpoint immediately. Disabling stops accepting requests, invalidates pending approvals, cancels outstanding MCP operations where possible, and stops the endpoint.

Committed application operations retain their normal completion/cancellation semantics.

MCP initialization must follow normal application initialization and use existing services. It must not require the QML views to be instantiated, but operator approval requires an available UI.

## 3. Transport and Authentication

Use stateless Streamable HTTP as implemented by the pinned `yahat-cpp` revision.
The inspected revision declares `McpServer::protocol_version = "2026-07-28"`:
exact POST routing, `server/discover`, JSON `tools/list`, and request-scoped SSE
`tools/call`. It implements no legacy initialize handshake, GET event stream,
session IDs, Tasks, or subscriptions. Application operation-status tools do not
imply MCP Tasks support.

Every HTTP request is authenticated. Clients must send the revision's required
metadata and matching routing headers, JSON content type, and Accept values for
JSON and SSE. Pin a compatible Python client. Verify discovery, listing, calls,
errors, asynchronous approvals, disconnects, and version/header validation against
the fetched dependency before claiming interoperability. Do not assume legacy
MCP clients work. Repeat these checks when updating the dependency pin.

**No MCP session management.**

Every request is independently authenticated using its API key. Authorization, audit attribution, rate limits, and approval handling derive from that key.

HTTP connections and SSE streams are transport details, not persistent security identities. Any transport-level request identifiers are used only for request correlation.

The server binds to loopback by default. Validate HTTP `Origin` where applicable, reject unauthorized cross-origin access, and keep API keys out of URLs and logs.

Remote access is outside v1 scope. Binding to non-loopback interfaces must be rejected and must not be offered until an authenticated, encrypted remote-access design is specified.

### 3.1 API keys

Provide a Settings interface to create, inspect, modify, and revoke MCP API keys.

Each key has:

- Stable internal key ID.
- User-assigned name.
- Cryptographically generated secret.
- Creation time.
- Optional expiration.
- Last-used time.
- Enabled/revoked state.
- Allowed local identities.
- Capability permissions.

The full secret is displayed once at creation. Persist only a secure verifier/hash.

Key IDs are authoritative; names are presentation and audit metadata.

Revocation takes effect immediately for new requests and pending operations. Running operations are cancelled where supported.

Authentication and authorization are checked for every request.

### 3.2 API-key capabilities

Initial capabilities:

| Capability | Authority |
|---|---|
| `read_only` | Restricts key to permitted read operations |
| `read_status` | Read authorized application status/readiness |
| `read_identities` | List accessible identities and their public sharing information |
| `read_contacts` | List/inspect contact metadata |
| `read_profiles` | Read locally available authorized profile contents |
| `read_presence` | Inspect locally available presence/connection state |
| `read_conversations` | List conversation metadata without content previews |
| `read_events` | Retrieve authorized operational events |
| `read_diagnostics` | Retrieve sanitized high-level diagnostics |
| `reject_contacts` | Reject contact requests |
| `remove_contacts` | Remove contacts |
| `send_messages` | Send messages |
| `create_contacts` | Add and approve contacts |
| `create_identities` | Create local identities |
| `read_message_content` | Read conversation contents |
| `read_files` | Access authorized files |
| `send_files` | Offer and send files |

`read_only` is a restrictive mode, not permission to read everything.

Additional tool capabilities may be introduced alongside new operations.

Each key is associated with one or more local identities. A new key defaults to the identity selected during creation.

Operations affecting another identity require explicit authorization for that identity.

API keys cannot manage API keys, grant themselves additional permissions, or modify their own authorization policies through MCP.

## 4. Authorization Model

Authorization consists of three independent layers:

1. API-key capabilities and identity access.
2. Identity/contact MCP policies.
3. MCP operation approval.

Every applicable layer must authorize execution.

A denial at any layer rejects the request.

Authorization is centralized and applies before application services execute an operation.

### 4.1 Identity and contact policies

Each local identity defines default MCP access policies for operations involving contacts.

Each contact may override the identity defaults.

Policy values:

- `Allow`
- `Ask`
- `Deny`
- `Inherit` — contact overrides only.

Identity defaults are `Allow`, subject to API-key permissions and operation approval.

Initially, policies cover:

- Sending messages.
- Reading message content.
- Reading profile contents.
- Sending files.
- Reading files associated with the contact.

Additional contact-scoped permissions may be added as corresponding tools are implemented.

Resolution:

1. Apply the contact override when present.
2. Otherwise apply the identity default.
3. `Deny` rejects immediately.
4. `Ask` requires approval for each operation.
5. `Allow` permits evaluation by the regular MCP approval policy.

`Allow` does not bypass mandatory approval for mutable operations or sensitive reads.

A contact-level `Ask` forces individual approval even when a persistent `Allow always` decision exists for the MCP operation.

For operations affecting multiple contacts, all affected contacts must satisfy their respective policies. Any required approval must clearly identify the complete affected set.

Operations without a contact target use identity-level authorization and the regular MCP approval policy.

### 4.2 Operation approval

Every mutable production MCP operation and sensitive read must pass the central
approval manager. Sensitive reads initially include profile and message contents;
future file-content reads follow the same rule. Capabilities and contact `Allow`
alone are insufficient. Persistent decisions are supported for sensitive reads.

Supported decisions:

| Decision | Behavior |
|---|---|
| Ask | Request operator approval; default |
| Allow once | Authorize the pending operation only |
| Allow always | Persist approval for matching future operations |
| Deny | Reject pending operation |
| Deny always | Persist rejection for matching future operations |

Persistent decisions are associated with the API-key ID, local identity, tool, and authorization scope.

For contact-scoped operations, permanent approval supports:

- This contact.
- All contacts.

`All contacts` still respects explicit contact overrides.

`Deny always` follows the same scope rules as `Allow always`.

Any matching persistent denial takes precedence over all matching persistent
allows, regardless of specificity. A one-time allow or contact `Ask` cannot
override it; edit/revoke the denial in Settings first. Multi-contact operations
are denied as a whole if any target is denied. A permanent decision for selected
contacts is stored separately per contact; `All contacts` is an explicit broader
scope.

New operations default to `Ask` unless an applicable persistent policy exists.

Approval and denial policies must be inspectable, editable, and revocable in Settings.

Sensitive operations may impose stronger authorization requirements where explicitly defined.

### 4.3 Approval requests

The approval manager creates pending requests containing:

- Unique approval ID.
- API-key ID and display name.
- Local identity.
- MCP tool name.
- Validated, immutable operation arguments.
- Human-readable operation summary.
- Affected contacts/resources.
- Requested approval scope.
- Creation time and deadline.
- Current state.

Approval authorizes the exact validated operation, including arguments and targets.

Execution uses the stored immutable arguments.

Show complete validated operation arguments locally, including outgoing message
content where applicable, using safe plain-text presentation. Authentication and
cryptographic secrets are never operation arguments or approval UI details.
Exclude sensitive arguments from history, popup summaries, logs, and diagnostic
artifacts. Release pending argument storage when execution no longer needs it.

Application services revalidate relevant state and preconditions when execution begins.

Approval grants permission to attempt an operation, not a guarantee of success.

### 4.4 Asynchronous execution

Approval requests use asynchronous MCP execution supported by `yahat-cpp`.

The request remains pending until:

- Approved.
- Denied.
- Timed out.
- Cancelled.
- Invalidated by authorization changes.

Default timeout: 60 seconds, configurable.

Use monotonic deadlines.

Timeout or cancellation rejects the operation without executing it.

Pending requests have a single authoritative state transition:

```text
PENDING
  ├── APPROVED → EXECUTING → COMPLETED/FAILED
  ├── DENIED
  ├── EXPIRED
  └── CANCELLED
```

State transitions are atomic. Late responses cannot execute expired or cancelled operations.

Operator approval, cancellation, timeout handling, and MCP request execution must not block the Qt UI thread or MCP event loop.

The approval manager must support cooperative cancellation and bounded concurrency.

If fresh approval is needed and no operator UI is available, reject immediately
with `ApprovalUnavailable`. Matching persistent allows can authorize execution
without a UI. In explicitly activated isolated tests, the authenticated controller
serves as an available decision provider. Navigating away from Home does not make
the operator UI unavailable.

### 4.4.1 Durable operation identity and retries

Admitted operations and pending approvals survive transport disconnects. The
pinned library marks `ToolContext` cancelled on SSE disconnect, so the application
must own operation state and cancellation independently of HTTP coroutine lifetime.
Explicit cancellation, revocation, timeout, disabling, and shutdown retain their
normal effects. Stopping HTTP does not drain arbitrary application work.

Every approval-gated invocation supplies a bounded client idempotency key. Before
acknowledging admission or starting side effects, atomically associate it with a
durable locally generated operation ID. Scope deduplication to API-key ID, local
identity, and tool, and bind it to canonical validated arguments and all targets.
Different arguments under the same key produce `IdempotencyConflict`. A matching
retry returns the existing ID/state without another approval or side effect.
Protect persisted argument fingerprints against offline guessing of sensitive
content; do not persist plaintext arguments merely for deduplication.

Provide operation-status lookup using current authentication, original capability
requirements, and identity/contact access. Ordinary keys see only their own
operations. IDs are not credentials. Status returns sanitized state and object
references, never original arguments or sensitive read results. Content access
through referenced objects requires the normal sensitive-read authorization.

Persist operation identity, deduplication association, state, and application
commit references using existing persistence services. Coordinate side-effect
commit and operation recording transactionally where possible. On restart,
recover proven outcomes and cancel unexecuted approvals rather than reviving old
approvals/deadlines. For uncertain effects, retain deduplication evidence and
return `IndeterminateOperation`; require recovery before re-execution. Never
blindly replay an uncertain operation or promise exactly-once remote delivery.

Document a bounded retry-retention window. After expiry, reject old idempotency
keys explicitly instead of treating them as new work. Use server-issued retry
epochs or an equivalent bounded mechanism to recognize expired keys without
unbounded tombstones. Reject admission when records cannot be retained safely;
never evict unresolved evidence to make space.

### 4.5 Persistent policies and identity changes

Store persistent decisions and contact overrides using DarkSpeak's existing persistence infrastructure.

Policy changes take effect immediately.

Revoke or invalidate affected approvals when:

- An API key is disabled or revoked.
- The key loses a required capability.
- The key loses access to an identity.
- An affected identity/contact policy changes.
- A matching persistent approval/denial is added, edited, or revoked.
- MCP is disabled.

Each execution must verify current authorization before performing its first side effect.

Operations already executing follow their normal cancellation and transaction semantics.

## 5. MCP Tool Architecture

Use one production MCP server with shared tool/authorization infrastructure.
The pinned library freezes a shared catalog that is not filtered per caller.
Test builds therefore expose controller tools through a separate authenticated
catalog/route, using another `McpServer` on the same loopback listener if needed.
Ordinary-key discovery and help must not expose privileged test descriptors.

Conceptually:

```cpp
registerCoreMcpTools(server);

#ifdef DARKSPEAK_WITH_TEST_MCP
registerTestMcpTools(controller_server); // controller-authenticated catalog
#endif
```

Tool registration, schema validation, capability checks, authorization, approval, dispatch, and result handling should use shared infrastructure.

Recommended execution flow:

```text
HTTP request
    ↓
API-key authentication
    ↓
MCP schema/argument validation
    ↓
Capability and identity authorization
    ↓
Contact-policy evaluation
    ↓
Approval manager
    ↓
Application service
    ↓
MCP result / structured error
```

All tools have explicit argument bounds, output bounds, permissions, and mutability classification.

Asynchronous operations distinguish request acceptance from actual completion.

Use existing application identifiers and operation IDs for correlation.

Tool schemas should remain stable across internal refactoring. Internal model or database changes should not unnecessarily alter public MCP contracts.

### 5.1 Initial production tools

Implement the essential operations for contacts, profiles, presence, and messaging.

| Area | Operations |
|---|---|
| Application | Status, readiness, selected identity information |
| Identity | List accessible identities, retrieve public identity/contact-sharing information |
| Contacts | List, inspect, add, approve/reject requests, remove |
| Profiles | Retrieve locally available/authorized profile information |
| Presence | Inspect contact presence and connection status |
| Conversations | List conversations |
| Messages | List/read authorized messages, send messages |
| Events | Retrieve bounded, authorized event history |
| Diagnostics | Retrieve permitted high-level application state |

Names and schemas should follow the project's existing conventions.

All mutable tools and sensitive reads use the approval manager.

Initial tool-to-capability mapping (concrete names must preserve this mapping):

| Operation | Required capability | Approval |
|---|---|---|
| Application status/readiness | `read_status` | No |
| Selected identity, identity list/public sharing information | `read_identities` | No |
| Contact list/inspect | `read_contacts` | No |
| Contact add/approve request | `create_contacts` | Yes |
| Contact reject request | `reject_contacts` | Yes |
| Contact remove | `remove_contacts` | Yes |
| Read locally available profile contents | `read_profiles` | Yes |
| Inspect local presence/connection state | `read_presence` | No |
| List conversation metadata | `read_conversations` | No |
| List/read message contents | `read_message_content` | Yes |
| Send message | `send_messages` | Yes |
| Retrieve authorized operational events | `read_events` | No |
| Retrieve sanitized diagnostics | `read_diagnostics` | No |
| Operation status/retry | Original capabilities and current access | No new approval for status; retry never re-executes |

Metadata tools must not include content previews that bypass sensitive-read
approval. Status/diagnostics must exclude unauthorized identity/contact state.
Authentication and `read_only` grant no implicit read permissions. Filter before
pagination and do not reveal hidden counts/identifiers through cursor gaps.

Read tools require relevant capabilities and identity/contact permissions. Reading sensitive content requires explicit authorization even though it does not mutate state.

Return information actually available to the requesting identity through normal application state. MCP must not bypass peer privacy restrictions.

Remote reads that trigger protocol requests or disclose presence must be classified and authorized according to their actual effects.

### 5.2 Deferred production tools

The authorization framework must accommodate the following additions:

| Area | Planned functionality |
|---|---|
| File transfers | Offer, accept, reject, cancel, resume, inspect |
| Virtual file manager | Browse, inspect, import/export, move, delete authorized files |
| Profile editing | Update biography, images, attachments, visibility |
| Identity management | Create, configure, switch, manage identities |
| Contacts | Extended properties and access controls |
| Messaging | Deletion and additional conversation operations |
| Settings | Read/change authorized application settings |
| Diagnostics | Additional operational and performance information |

Keep this list as the v1 deferred-functionality backlog.

Phase 2 scenario P2-001 brings forward the minimal profile/bootstrap, public-bio
probe and file offer/acceptance/inspection subset it needs. See its implementation
dependencies in section 10.3's linked scenario; the remaining operations stay in
the backlog. Concrete production tool additions require owner review before
registration.

Implement associated capabilities and authorization policies when introducing each operation. The initial permission model already reserves `create_identities`, `read_files`, and `send_files`.

## 6. Home Pane — Operator Approvals

Integrate MCP approval requests into the existing Home pane.

MCP requests appear first, above contact requests, unread messages, and other events, because approvals are time-sensitive.

Each pending request displays:

- Requesting API-key name.
- Requested operation.
- Local identity and relevant contacts.
- Human-readable arguments/details.
- Remaining approval time.
- Available decisions and applicable scope.

Actions:

- Allow once.
- Allow always.
- Deny.
- Deny always.

For contact-scoped permanent decisions, offer `This contact` (or `These contacts`) and `All contacts`; store selected-contact decisions individually.

The user can dismiss or navigate away without approving. Pending requests continue until resolved or expired.

Expired, denied, cancelled, and completed requests leave the pending section and remain available through bounded event history.

Use the existing Home notification mechanisms for arrival and status changes.

Avoid blocking modal dialogs and automatic navigation.

Limit the number of simultaneous pending requests globally and per API key.

Duplicate or excessive requests must not flood the Home pane.

## 7. Status-Bar MCP Indicator

When MCP is enabled, display an MCP status-bar icon.

States:

| State | Appearance |
|---|---|
| Disabled | Hidden |
| Enabled/idle | Static |
| Active | Animated |

Activity includes MCP requests, pending approvals, and running operations.

Start animation immediately when activity begins.

Animation remains active during ongoing activity and is visible for at least three seconds after the most recent activity begins.

### 7.1 Activity popup

Clicking the icon opens a popup containing:

- Current active MCP requests/operations.
- Pending approval requests.
- Recently completed operations.
- Recently failed, denied, cancelled, or expired operations.

Display API-key name, operation, target summary, status, and relative time.

Distinguish active, completed, and failed operations visually.

Maintain a small bounded in-memory history for recent activity.

The popup provides buttons to open:

- MCP settings.
- API-key management.
- MCP approval/policy settings.
- Home approval requests, when pending.

Activity history excludes secrets and sensitive payloads.

The popup reflects actual server and operation state, rather than maintaining an independent source of truth.

## 8. Event Logging and Observability

Provide a separate bounded in-memory operational event stream for MCP operations
and important application state transitions. Preserve the optional persistent
journal in `doc/events.md`, including off-by-default recording, timestamp
precision, aggregation, filename preferences, and retention/clearing semantics.
Operational collection does not implicitly enable persistent journal recording.
Share domain producers where practical, with separate privacy/retention projections.

Ordinary keys can retrieve only their own MCP activity and authorized application
events under current identity/contact access. Never expose another key's activity,
approvals, name, or targets. Use opaque authorization-bound cursors; internal
sequence numbers must not reveal hidden events through gaps. Report actual
buffer loss without revealing counts of filtered events. Privileged test
controller collection is instance-wide. Operational events are not persisted by
default.

Each event should include, where applicable:

- Unique local event sequence number.
- Process/run identifier.
- Timestamp.
- Component and event type.
- Local identity.
- API-key ID/name for MCP-originated actions.
- Operation/request/correlation ID.
- Relevant application object identifiers.
- Result/status and sanitized error details.

Use monotonically increasing per-process sequence numbers for event ordering.

Support bounded retrieval using sequence cursors.

Report missing/dropped events when event-buffer limits are exceeded.

Useful MCP events include:

```text
mcp.request.received
mcp.request.rejected
mcp.approval.requested
mcp.approval.granted
mcp.approval.denied
mcp.approval.expired
mcp.operation.started
mcp.operation.completed
mcp.operation.failed
mcp.operation.cancelled
```

Also expose relevant existing contact, presence, messaging, connection, and file-transfer events.

Logs and diagnostic output must exclude API-key secrets, cryptographic secrets, and private message/file contents by default.

Use bounded buffers, efficient event generation, and asynchronous collection.

Avoid expensive diagnostic work on latency-sensitive application paths.

The same event infrastructure supports:

- Home notifications.
- Status-bar activity.
- MCP diagnostics.
- Python integration tests.
- Codex investigations.

## 9. Test-Only MCP Extensions

Test extensions are available only when compiled with `DARKSPEAK_WITH_TEST_MCP` and explicitly activated for an isolated test instance.

They extend the production MCP interface with controlled observability and test operations.

Initial extensions:

- Inspect detailed application/subsystem state.
- Inspect pending approval requests.
- Approve or deny a pending request.
- Inspect structured event streams.
- Inspect bounded queues, counters, and relevant state transitions.
- Retrieve additional diagnostic information needed for test assertions.

Test approval tools invoke the production approval manager and its normal state transitions.

The Python framework may automatically approve requests through these tools when the scenario is not testing authorization behavior.

Approval tests explicitly exercise all decisions, scopes, cancellation, and timeout behavior.

Test-only tools require separate test authorization and must not be discoverable
or callable in production builds or ordinary-key catalogs in test builds.

Provide test-build-only bootstrap, explicitly activated with an isolated profile
root, to initialize normal services, independent identities, production test keys,
and a separate cryptographically generated controller credential without QML
interaction. Refuse production profiles and unsafe/shared roots. Isolate all
configuration, storage, secrets, Tor state, endpoints, and artifacts. Deliver
secrets via owner-only files or protected local channels, never command arguments,
URLs, or logs. The controller can inspect and resolve every approval in its test
instance, including decisions/scopes, through the production approval manager.
It cannot override capability/contact denials, bypass validation, or affect other
instances.

Prefer external test utilities for process killing, filesystem manipulation, transport disruption, and malicious protocol traffic.

Test instrumentation must not weaken normal cryptographic verification, protocol validation, or security boundaries.

## 10. Python Acceptance-Testing Framework

Use Python, `pytest`, and `pytest-asyncio`.

The framework starts and controls multiple independent DarkSpeak instances and communicates through MCP.

Each instance has isolated:

- Identity and keys.
- Configuration/database.
- File storage and transfer staging.
- Tor configuration and data directory.
- Network endpoints.
- MCP endpoint and API keys.
- Logs and event buffers.

Test isolation must prevent connections to the user's normal DarkSpeak instance and access to production data.

Use real application processes, C++ services, protocol implementations, and storage.

Phase 2 includes an isolated local Tor network before mandatory peer-network
tests and CI. Use real Tor and onion services, never a direct-transport shortcut.
Do not use the user's Tor daemon or public Tor by default. Optional public-Tor
tests require explicit selection and isolated application/Tor profiles.

Keep public-Tor integration tests separate from the mandatory local suite.

### 10.1 Framework responsibilities

Implement reusable fixtures/helpers for:

- Creating and starting test instances.
- Generating independent identities.
- Provisioning test API keys.
- Establishing MCP connections.
- Waiting for application and Tor readiness.
- Managing test approval decisions.
- Performing bounded asynchronous state/event assertions.
- Controlling process lifecycle and restarts.
- Generating reproducible test files.
- Collecting diagnostic artifacts.
- Cleaning up test environments.

Support two or more peers per scenario.

Tests use event/state conditions and bounded deadlines instead of fixed sleeps.

Negative assertions require suitable protocol completion milestones or bounded observation windows.

### 10.2 Codex workflow

Codex is responsible for developing, extending, and investigating tests.

Recommended workflow:

1. Read the feature specification and relevant implementation.
2. Define expected behavior and acceptance criteria.
3. Start isolated DarkSpeak instances.
4. Exercise the scenario interactively using MCP.
5. Investigate and fix discovered problems.
6. Convert the verified scenario into a Python integration test.
7. Repeat the Python test to establish reliability.
8. Add it to the permanent regression suite.

Tests must assert specified behavior rather than simply reproduce observed behavior.

Regression tests execute without Codex, an LLM, or external AI services.

When a regression is discovered, add or update a test that detects it.

### 10.3 Initial acceptance tests

The first Phase 2 peer scenario is
[P2-001: two-peer happy path](../tests/mcp/scenarios/001-two-peer-happy-path.md).
It defines two fresh application instances with one identity each, public-bio
previews in both directions, one connection request and acceptance, established
connections on both ends, profile/avatar exchange, a text conversation and
verified media transfers in both directions. Its checked-in
[public-domain fixtures](../tests/assets/mcp/README.md) require no runtime internet
downloads. The [test infrastructure](../tests/mcp/README.md) now implements this
scenario using isolated real application services, a separate controller, private
Tor and a Python runner with fatal/recoverable failure classification. The wider
Phase 2 matrix remains to be implemented.

Implement these first:

| Test | Expected behavior |
|---|---|
| Application readiness | Independent instances initialize correctly |
| Contact exchange | Contacts can be added and approved |
| Two-peer happy path (P2-001) | Public bios, contact acceptance, established connections, avatars, conversation and verified JPEG/audio transfers in both directions |
| Contact rejection | Rejected contacts remain unauthorized |
| Profile visibility | Approved contacts see permitted data |
| Profile privacy | Unauthorized contacts cannot retrieve restricted data |
| Presence | Online/offline transitions are observed |
| Messaging | Messages reach the intended recipient |
| Message persistence | Messages survive restart |
| Reconnection | Messaging works after disconnect/restart |
| Event logging | Relevant transitions are recorded and correlated |
| Approval allow | Approved operation executes exactly once |
| Approval deny | Denied operation produces no side effects |
| Approval timeout | Unanswered request expires without execution |
| Persistent approval | Matching operations follow stored policy |
| Contact overrides | Contact policies override identity defaults |
| Key capabilities | Unauthorized operations are rejected |
| Key revocation | Revoked keys lose access immediately |
| Sensitive reads | Approval required; persistent decisions supported |
| Conflicting policies | Any matching persistent denial wins |
| Policy updates | Affected pending approvals are invalidated |
| Missing operator UI | Fresh approval rejected; persistent allows still apply |
| Multi-contact decisions | Full targets displayed; selected-contact policies stored separately |
| Disconnect/retry | Work survives; matching retry creates no duplicate |
| Restart recovery | Durable IDs preserved; uncertain effects are not replayed |
| Retry conflict/expiry | Changed arguments and expired keys rejected explicitly |
| Controller isolation | Ordinary keys cannot discover/call privileged tools |
| Event privacy | Filtering, bounded loss reporting, persistent journal settings preserved |
| Reconfiguration | Failed bind preserves endpoint; non-loopback rejected |
| Overload | Safe rejection without starving status/approval resolution |
| Artifacts | Retained outputs contain no sensitive payloads or secrets |

### 10.4 Deferred acceptance tests

Extend coverage as implementation progresses:

**File transfers:** Basic offer/acceptance, completion and checksum verification
are required by P2-001 in Phase 2. Extend them with rejection, cancellation,
progress behavior, resume and persistence in subsequent scenarios.

**File integrity:** Corrupted files never become available. Resumed transfers recalculate and verify the complete file checksum before publication.

**Recovery:** Interrupted transfers, process crashes, staging recovery, and atomic publication behavior.

**Security:** Invalid signatures, malformed protocol messages, replay attempts, unauthorized access, oversized inputs, and hostile peers.

**Concurrency:** Simultaneous contacts, messages, transfers, approvals, cancellation, and policy changes.

**Resource limits:** Bounded queues, memory, event buffers, file sizes, and operation concurrency.

Adversarial protocol tests use an independent malicious-peer implementation.

### 10.5 Failure artifacts

On failure, preserve:

- Test name, parameters, and random seed.
- Application build identifiers.
- Relevant sanitized configuration.
- Process exit status and crash details.
- Sanitized MCP request/response trace.
- Structured events from every instance.
- Application logs.
- Sanitized relevant final application-state snapshots.

Sanitize all retained artifacts, including CI uploads, exception output, process
arguments/environment, logs, and snapshots. Exclude credentials, sensitive
arguments, message/profile/file contents, and raw memory dumps. Preserve safe
correlation IDs, redacted schemas, and outcomes. Additional sensitive local
capture requires explicit user opt-in, owner-only storage, documented retention
and cleanup, and exclusion from automatic CI upload. Authentication and
cryptographic secrets remain excluded even with diagnostic opt-in.

Artifacts must be sufficient for Codex to investigate failures without requiring immediate reproduction.

Support repeated execution of failed tests and retention of individual results.

### 10.6 CI

Integrate the regression suite with CMake/CTest and GitHub Actions.

Provide a straightforward local command for running the complete integration suite or a selected scenario.

Tests must have explicit deadlines and cleanup behavior.

Parallel execution may be added after process, filesystem, and network isolation are verified.

Preserve failure artifacts in CI.

## 11. GUI Coverage

MCP verifies C++ application services, state, protocol behavior, and persistence. It does not prove that QML views display or update correctly.

Add a small separate GUI acceptance layer using Qt Quick Test or suitable Qt automation.

Use MCP to prepare application state, then verify:

- Contact-list updates.
- Profile rendering and visibility.
- Conversation navigation.
- Message display.
- Transfer progress display.
- Home approval presentation.
- Status-bar MCP activity.
- Approval UI interactions.

Keep GUI tests focused on rendering, bindings, navigation, and user interaction rather than repeating the full integration suite.

## 12. Security and Resource Requirements

All MCP input is untrusted, including requests from authenticated API keys.

Apply existing DarkSpeak validation, authorization, resource limits, and security policies.

Specific requirements:

- Cryptographically secure API-key generation.
- Per-request authentication.
- Per-key and per-identity capability enforcement.
- Central approval enforcement for all mutable production operations and sensitive reads.
- Immutable approved arguments.
- Atomic approval-state transitions.
- Immediate key revocation and policy updates.
- Bounded request, approval, and event queues.
- Bounded request sizes, responses, and execution times.
- Rate limits per API key.
- Cancellation and timeout handling.
- Safe filesystem boundaries for file operations.
- Redaction of credentials and private data.
- No blocking of Qt UI or networking event loops.
- Test-only functionality excluded from production binaries.

The approval manager must also handle overload safely. When pending-request limits are reached, reject new requests with a structured error.

Shutdown and MCP disabling must resolve pending requests and release associated resources.

### 12.1 Resource-limit selection

Codex chooses and documents concrete implementation limits, with rationale and
boundary/overload acceptance coverage. Initial budgets below are local ceilings;
stricter domain/protocol limits still apply. Bound allocations before processing
input/building results, not merely after parsing or serialization.

| Resource | Initial budget |
|---|---|
| HTTP body / decoded MCP input | 256 KiB each; initially reject compressed requests |
| MCP serialized result including cumulative SSE progress | 1 MiB |
| Help topic / list page | 32 KiB / 64 items |
| Concurrent HTTP connections / in-flight MCP calls | 64 / 32 globally |
| Admitted nonterminal operations | 64 globally / 8 per key |
| Pending approvals, within operation budget | 32 globally / 4 per key |
| Tool-call admission | 600/minute globally / 120/minute per key |
| Unauthenticated HTTP admission | 120/minute globally, burst 20 |
| Operational event buffer | 2,048 events and 4 MiB, whichever fills first |
| Activity popup history | 100 sanitized records |
| Approval timeout | 60 seconds, configurable 1–300 seconds |
| HTTP header/body receipt deadline | 10 seconds |
| Initial active execution deadline | 120 seconds excluding approval wait |
| Terminal operation record retention | 24 hours; at most 10,000 records / 16 MiB |
| Client idempotency key | 128 ASCII bytes |

Also explicitly bound headers, JSON nesting, collections, string lengths, API-key
counts, and persisted policy counts during implementation. Reserve admission
capacity for operation status and controller decisions; waiting approvals must
not exhaust all execution slots. Never silently evict policies or unresolved
operation evidence. If promised retry retention cannot be maintained, reject new
admission. Deadline expiry requests cooperative cancellation; committed/uncertain
effects must not be reported as safely retryable failures.

## 13. Implementation Sequence

### Phase 1 — MCP integration

- CMake configuration and runtime enable/disable.
- Pinned stateless Streamable HTTP integration through `yahat-cpp`, with compatibility checks.
- API-key generation, persistence, authentication, and capability enforcement.
- Central asynchronous approval manager, durable operation IDs/status, and idempotent retries.
- Identity defaults and contact overrides.
- Home-pane approval UI.
- Status-bar activity indicator and popup.
- Structured MCP events.
- Initial identity, contact, profile, presence, and messaging tools.

### Phase 2 — Regression tests

- Test-only MCP registration and approval simulation.
- Isolated bootstrap, separate controller credential/catalog, and multi-instance process management.
- Isolated local Tor infrastructure for mandatory peer-network tests and CI.
- Python MCP client helpers and pytest fixtures.
- Structured event assertions and failure artifacts.
- Initial contact, profile, presence, messaging, and approval tests.
- First define P2-001, then implement its basic file offer/acceptance, bounded
  transfer inspection and bidirectional receiver-integrity checks. Review the
  required MCP additions; the Phase 1 catalog does not yet expose these operations.
- Focused GUI checks for approval presentation/actions, settings, and activity.
- CI integration.

Focused authorization, persistence/recovery, transport-compatibility, and UI tests
accompany Phase 1 rather than waiting for the regression framework. See
`doc/mcp.md` for the initial implementation contracts and verification commands.

Deferred backlog (not a third implementation phase):

- Extended file-transfer MCP operations and corruption, resume, and recovery tests
  beyond the basic P2-001 transfer/integrity requirements.
- Additional identity, profile, contact, and file-manager operations.
- Independent adversarial protocol tests and extended GUI coverage.

## 14. Acceptance Criteria

The initial implementation is complete when:

1. Production MCP can be compiled in or excluded entirely.
2. Users can enable/disable MCP without restarting DarkSpeak.
3. API keys identify callers and enforce configured capabilities and identity access.
4. Every mutable production MCP operation and sensitive read is authorized through the asynchronous approval manager.
5. Approval supports one-time decisions, persistent decisions, and contact-specific scopes.
6. Identity defaults and contact overrides resolve consistently.
7. Pending approvals appear at the top of Home and expire safely.
8. The status-bar icon indicates MCP activity and exposes recent operations.
9. Core MCP tools use existing application services and produce structured events.
10. Test builds provide isolated approval simulation and diagnostics.
11. Python tests can orchestrate independent DarkSpeak peers and verify contacts, privacy, presence, messaging, persistence, and authorization.
12. The mandatory suite runs on isolated local Tor without an AI model and retains sanitized diagnostics.
13. Durable IDs/status and idempotent retries preserve operations across disconnects/restarts without uncertain re-execution.
14. Matching persistent denials win; policy changes invalidate affected pending approvals.
15. Ordinary keys cannot discover/call controller tools; release builds exclude them.
16. The dependency is fetched at a pinned revision without local-checkout assumptions; compatible clients pass transport checks.
17. Non-loopback binds are rejected; failed reconfiguration preserves the working endpoint.

The implementation should favor shared infrastructure, stable interfaces, small bounded components, and existing DarkSpeak conventions. New capabilities should extend the MCP tool registry and authorization model without requiring architectural changes.
