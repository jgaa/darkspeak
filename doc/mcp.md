# Local MCP interface

MCP is optional and disabled by default. Configure with
`-DDARKSPEAK_WITH_MCP=ON`, then enable it in Settings → MCP. The default endpoint
is `http://127.0.0.1:17839/mcp`. Literal loopback addresses are mandatory. Every
request requires `Authorization: Bearer <secret>`; credentials must never appear
in a URL. Browser Origins are rejected.

The dependency revision is defined in the root CMakeLists.txt. CMake downloads it
and applies `cmake/yahat-hardening.patch` to that fetched source. No sibling
checkout or installed yahat library is used. Its Boost 1.81+ components
(program_options, coroutine, context, chrono, json, url), OpenSSL and zlib must be
available as build dependencies. Metrics, library examples and library tests are
disabled. MCP-disabled builds do not fetch or link yahat.

For offline builds, prepare the pinned source using CMake's standard
`FETCHCONTENT_SOURCE_DIR_YAHAT` mechanism. The same consumer patch is applied.
Flatpak source preparation includes that revision; its existing build keeps MCP
disabled by default. An MCP-enabled Flatpak build also needs the listed Boost
libraries in its SDK/build environment. After changing the root pin, run
`python3 scripts/update-mcp-flatpak-pin.py` to regenerate its manifest entry.

## Client contract

This pin implements stateless MCP **2026-07-28**, without initialize/sessions,
GET streams, Tasks, subscriptions, resources or prompts. Tested Python client
versions are pinned in `tests/mcp/requirements.txt`. Use `ClientSession.discover`,
`list_tools` and `call_tool` with `streamable_http_client`; see
`tests/mcp/client_smoke.py`. An older MCP SDK is not interchangeable.

Each request includes a JSON-RPC ID and params._meta containing
`io.modelcontextprotocol/protocolVersion` and
`io.modelcontextprotocol/clientCapabilities`. Send Content-Type application/json,
Accept application/json plus text/event-stream, MCP-Protocol-Version, Mcp-Method,
and Mcp-Name for tool calls. Routing headers must match the body. Discovery and
listing return JSON; calls return request-scoped SSE.

`darkspeak_help` provides authorization, usage and retry details for every tool.
Schemas and capability classification share the production tool registry.

| Tool | Capability | Approval |
|---|---|---|
| status | read_status | No |
| identities, identity_info | read_identities | No |
| contacts, contact, contact_requests | read_contacts | No |
| contact_add, contact_approve | create_contacts | Yes |
| contact_reject | reject_contacts | Yes |
| contact_remove | remove_contacts | Yes |
| profile | read_profiles | Yes |
| presence | read_presence | No |
| conversations | read_conversations | No |
| messages | read_message_content | Yes |
| message_send | send_messages | Yes |
| events | read_events | No |
| diagnostics | read_diagnostics | No |
| operation_status | Original capabilities and current access | No new approval |

`read_only` restricts mutations and grants no access itself. Reserved capabilities
for deferred tools confer no additional functionality. There are no key/policy
administration or test-controller tools on the production catalog.

For the compile-time-excluded isolated test bootstrap, separate controller and
private Tor regression commands, see [the test infrastructure](../tests/mcp/README.md).
Test-build-only probe/file operations share this approval manager; they are not
registered on normal production catalogs.

All identity/contact/conversation arguments use canonical UUIDs without braces.
`contact_requests` returns opaque request handles, which are bound to the caller
and identity. List tools accept an optional opaque cursor and limit (1–64,
default 32). Cursors cannot be reused across callers, identities, tools or
conversations. Results expose public UUIDs/random message IDs instead of internal
row offsets. Event cursors are also bound to the process run. Current access is
checked on every retrieval; no hidden counts or raw internal sequences are
returned. Buffer-loss reporting uses bounded per-key/identity watermarks for
visible events; eviction of another key's hidden activity does not report a
loss. Authorization changes invalidate event cursors. Metadata lists contain no message/profile previews. `profile` reads
the locally available biography; it does not trigger a remote fetch.

`message_send` takes 1–8 distinct contact UUIDs and 1–65536 UTF-8 bytes. Recipients
are canonicalized into sorted order. Peer preferences are rechecked before local
admission. A completed send means local message persistence/queueing, not delivery
to the peer. Multi-contact sends may have partial committed outcomes after an
error or crash; their recorded references must be examined before recovery.

Queued outbound messages and files initiate a connection to an accepted,
unblocked disconnected contact, even when contact auto-connect is disabled.
This also applies to GUI sends. An offline identity stays offline; its queued
connection request waits until that identity comes online. Concurrent sends
share one connection attempt, and a manual disconnect cancels a pending attempt.
Files may be imported locally before peer preferences are available; no file
offer or message is sent until preferences arrive and the peer's policy permits
it. Connection failures do not start an automatic retry loop.

## Authorization and local controls

Settings creates, inspects, updates and revokes keys. Secrets contain 256 random
bits, are shown once, and are persisted only as cryptographic verifiers. Each key
has explicit identity and capability lists, an optional expiration and last-used
metadata. Names are presentation only.

Identity/contact policies, persistent operation decisions, and key permissions
are independent requirements. Any matching persistent denial wins. Contact Ask
requires a fresh decision even when a persistent allow exists. Selected-contact
permanent decisions are stored individually; all-contact scope is explicit.
Policy/key changes cancel affected pending/approved operations. Execution checks
current permissions and resource state again before effects.

Home displays full immutable arguments using plain text JSON, with invisible
formatting/bidirectional controls escaped, including all message
recipients and content. For incoming contact decisions it includes a snapshot of
the request; a changed request invalidates the attempt before dispatch. History,
events and diagnostics omit those arguments and sensitive read results. The
status popup uses a separate sanitized projection. No QML view needs to exist to
start the endpoint; a fresh approval without an available UI fails immediately
with ApprovalUnavailable. Persistent allows still work without a UI.

## Durable operations and retries

Before a new gated call, obtain `retry_epoch` from status. Supply
`idempotency_key = retry_epoch + ':' + unique_printable_ASCII_token`, at most 128
bytes. The server persists random epochs, rotates them after 24 hours (or clock
rollback), and never reuses a retired epoch. A retired epoch cannot admit new
work, but matching retained records can still be retried.

Deduplication is scoped to key ID, identity and tool. Canonical arguments are
protected by a keyed hash whose secret lives in the existing OS vault. Operation
records contain no argument plaintext solely for retries. They are committed
before approvals or effects. An unchanged retry returns the existing operation
ID and sanitized state; changed arguments produce IdempotencyConflict. Sensitive
results are delivered only to the original live invocation. Reading content
again requires a separate authorized invocation.

HTTP disconnect cancels only the transport wait. Approval and admitted work are
owned by the application. Revocation, denial, expiration, disabling and shutdown
resolve pending work. Approval deadlines are monotonic. A late decision cannot
revive a terminal operation.

On restart, pending/approved records become Cancelled. Executing records become
IndeterminateOperation and are never replayed. Outgoing message insertion and
its operation commit evidence share a transaction before publication/queueing.
A proven final commit remains Completed even if subsequent notification fails.
Other existing services retain their transaction boundaries; uncertainty across
those boundaries is reported rather than retried automatically.

Terminal records are retained for 24 hours after admission, subject to the
specified count/byte admission ceilings. Unresolved evidence is never evicted.
New calls are refused when retention cannot be honored. Expired tokens are
rejected even after their records have been pruned. Operation IDs are not
credentials. Status requires the originating key and its current original
permissions; it returns state and safe references, never original arguments or
sensitive contents. Completed removal status remains available for its deleted
contact to the originating authorized key.

The independent schema is version 1, with a fixture in
`tests/fixtures/mcp/schema-v1.json`. Unsupported versions fail closed. The
optional persistent event journal retains its existing recording preferences;
MCP operational events are an independent in-memory projection.

## Resource limits

The budgets in specs/mcp_and_testing.md remain ceilings. Concrete additional
limits and reservations are:

- 16 KiB HTTP headers; 16 JSON levels; 64 members/items per container; 128-byte
  object keys; 65536-byte JSON strings. Compression is rejected before decoding.
- 64 connections, 32 in-flight calls, with 8 slots reserved for operation_status.
  Ordinary calls share 540 calls/minute; status has 60 reserved calls/minute.
- HTTP admission also reserves control capacity: ordinary/status token buckets
  replenish at 540/60 per minute globally and 100/20 per minute per key.
  Unauthenticated traffic replenishes at 120/minute, burst 20.
- 64 keys, 4096 persistent operation decisions, 4096 contact-policy entries,
  4096 simultaneously watched contact objects, and 8 recipients per send.
- 64 admitted operations globally/8 per key, at most 32 pending approvals/4 per
  key. The transport's reserved slots may impose a tighter effective wait limit.
- List construction stops at 320 KiB before transport duplication into text and
  structured content; full serialized/SSE output remains bounded at 1 MiB.
- Operational storage: 2048 events/4 MiB; recent sanitized activity: 100 records.
  Incoming request snapshots: 32 KiB. Internal cursor payloads have fixed width.
- 10-second HTTP receipt timeout; approvals default 60 seconds, selectable 1–300.
  Active execution has a 120-second monotonic deadline, checked between local
  steps and recipients; approval wait is excluded. Cancellation is cooperative
  and does not undo committed effects. Execution performs bounded local
  admission/read/commit work and never waits for peer delivery; there are no
  long-running file tools in this phase.

Errors are safe codes such as AccessDenied, ApprovalUnavailable,
InvalidArguments, InvalidSharingId, InvalidCursor, OperationLimit, RetentionLimit,
IdempotencyConflict, IdempotencyExpired and PersistenceFailed. Unexpected
exceptions return InternalError; exception messages and request payloads are not
logged by the adapter.

## Focused verification

Only new test targets and their dependencies need building; application builds
remain the owner's responsibility under agents.md.

```sh
cmake -S . -B /tmp/darkspeak-mcp-tests -DDARKSPEAK_WITH_MCP=ON -DBUILD_TESTING=ON
cmake --build /tmp/darkspeak-mcp-tests --target mcp_policy_test mcp_service_test mcp_ui_test
python3 -m venv /tmp/darkspeak-mcp-client
/tmp/darkspeak-mcp-client/bin/python -m pip install -r tests/mcp/requirements.txt
DARKSPEAK_MCP_TEST_PYTHON=/tmp/darkspeak-mcp-client/bin/python \
  ctest --test-dir /tmp/darkspeak-mcp-tests -R '^mcp_' --output-on-failure
```

Without DARKSPEAK_MCP_TEST_PYTHON the SDK case explicitly skips; C++ cases still
run. The test executable uses disposable profiles, vault/config/cache roots and
loopback endpoints. It never starts Tor. SDK credentials are delivered over
stdin, and errors retain only exception type. Tests verify resolution, approval
transitions, timeouts, revocation, multi-contact decisions, live HTTP disconnects,
retry conflicts/expiry, uncertain recovery, cursor isolation, overload, endpoint
replacement, actual QML argument rendering and settings rollback.

Phase 2 adds isolated application bootstrap/controller catalogs, a private real
Tor network, multi-process pytest regression scenarios and CI. File-transfer
operations and the remaining deferred tools stay in the specification backlog.
