# P2-001: Two peers, contact acceptance, biographies and media

Status: implemented by [the Python scenario](../scenario.py), using the
[isolated runtime and private Tor infrastructure](../README.md).

This is the first mandatory Phase 2 peer scenario. It runs two real DarkSpeak
processes, A and B, each creating exactly one new identity. It uses real Tor and
onion services in an isolated private Tor network, with no public Tor, AI service,
or runtime asset downloads. Execute sequentially before introducing concurrency.

## Fixture and isolation

- Create a fresh owner-only temporary run root and separate A/B profile,
  configuration, vault, managed storage, downloads, Tor data and artifact roots.
  Never reuse a normal installation's settings, credentials or Tor daemon.
- Require both MCP build flags, explicit test activation and validated isolated
  roots. Start each application with normal production services. Do not seed
  contacts, conversations or received data directly into storage.
- Create one identity per process through the isolated bootstrap using the normal
  identity creation service. Assert exactly one identity locally and distinct
  public signing keys, identity IDs and v3 onion addresses across A and B.
- Use independent ordinary keys restricted to their own identity and the required
  capabilities. Use a different controller credential/catalog for each process.
  Provision credentials through owner-only files or protected channels; never
  command arguments, URLs or logs. Do not copy a vault between processes.
- Set both identities online, biography visibility `PUBLIC`, and public avatars
  enabled. Enable ordinary messages and explicit acceptance of `image/jpeg` and
  `audio/ogg` files at both peers, subject to normal domain limits. This scenario
  tests permitted exchange; restricted visibility belongs in a separate scenario.
- Set A's display name to `Peer A` and bio to `Peer A — Earth observer.`; set B's
  name to `Peer B` and bio to `Peer B — Lunar explorer.`. Import `earth.jpg` as A's
  avatar and `apollo11.jpg` as B's avatar through normal avatar normalization.
  Each resulting avatar must be nonempty and at most 128 × 128 pixels.
- Copy the three checked-in [assets](../../assets/mcp/README.md) into each
  instance's permitted input root. Check their manifest sizes and SHA-256 before
  launching processes. Source fixtures remain read-only; received files must
  appear in each receiver's managed storage, never over the source fixtures.
- Disable optional persistent journaling. Collect bounded operational events;
  collecting these must not enable the persistent journal.

MCP approval is distinct from accepting a peer's connection request or file
offer. The harness allows each expected MCP approval **once**, using the owning
instance's controller and the production approval manager. Verify its tool,
identity, targets and immutable arguments first; never automatically approve an
unexpected request. Ordinary calls retain capability/contact-policy enforcement.
The test controller counts as the explicit test operator for this run; it must
not weaken the production rule for an unavailable operator UI.

## Ordered assertions

| Step | Action | Required observation |
|---|---|---|
| 1. Application readiness | Start A and B and authenticate their ordinary clients. | Each application reports services/database/MCP ready and exactly its own identity. Readiness does not require Tor or a peer connection. Ordinary discovery exposes no controller descriptors. |
| 2. Tor readiness | Bootstrap the private Tor network, then each application's Tor client and onion service. | Local Tor bootstrap and onion service availability succeed separately from application readiness. No public directory authority, relay, fallback or bridge is used. Onion readiness does not imply contact establishment. |
| 3. Initiator preview | Give A only B's public sharing information; use the normal public-bio probe. | A obtains B's exact bio through the authenticated peer protocol. The probe does not approve B, create a conversation, or send a connection request. |
| 4. One connection request | A adds B and sends a connection request with a fixed synthetic greeting. | B observes exactly one unresolved incoming request from A's authenticated identity. A is awaiting acceptance. Neither party treats transport authentication as contact approval. |
| 5. Recipient preview | Inspect B's pending request and retrieve A's public bio through the normal peer workflow. | B obtains A's exact bio before accepting. A's public bio is available to B and B's public bio remains available to A; no database injection or controller-supplied peer content is used. A read of local cached profile data alone is insufficient to initiate a remote fetch. |
| 6. Accept | B accepts that request once using the ordinary `contact_approve` path, with a separate MCP allow-once decision. | B durably approves A; A validates the correlated acceptance and durably approves B. Exactly one contact per peer, no unresolved request, and no duplicate contact after normal auxiliary probe/reconnect activity. |
| 7. Established state | Wait for both contacts' current approved, authenticated connections and online presence. | A sees B established/online and B sees A established/online. A queued request or a live TCP/Tor connection alone is insufficient. |
| 8. Full profile exchange | Read the locally received authorized profiles at each peer, allowing sensitive MCP reads once. | Exact remote names and biographies match. Each received avatar matches the corresponding sender's normalized avatar dimensions and protocol hash over `red || green || blue`, rather than the JPEG file hash. Avatar arrival is auxiliary and must not gate acceptance. |
| 9. Start conversation | A sends `Hello B — P2-001/A`; B replies `Hello A — P2-001/B` through ordinary messaging. | One direct conversation per side, scoped to the correct local identity and opposite contact. Both exact texts arrive once at the intended receiver, with correct direction and correlated message references. Sender receipt state confirms receipt, not display/read. Local send completion is insufficient. |
| 10. A → B media | A offers `earth.jpg`, `apollo11.jpg`, then `sos.ogg`, one at a time, in the conversation. B explicitly accepts each offer. | For each file, B observes an offer from A, then normal transfer/verification/publication; both ends reach successful terminal state. Verify file ID correlation, direction, size, content-derived MIME and exact SHA-256 of the published receiver bytes against the manifest. |
| 11. B → A media | B offers the same three fixture types to A in the same order. A explicitly accepts each offer. | The same assertions pass independently in reverse. Separate transfer IDs and receiver storage are used; neither direction's success substitutes for the other. |
| 12. Final state | Query bounded conversation, request, transfer and event state at both peers. | Exactly two text messages per local conversation (one outgoing, one incoming), three outgoing and three incoming completed transfers per instance, one approved contact each, no unresolved requests/approvals/transfers, and no transfer failure. File rows are counted separately from text rows. Persistent journaling remains disabled. |
| 13. Cleanup | Shut down both applications and all private Tor network processes, then remove the run root. | Child processes are reaped, listeners released and private profiles/vaults/Tor keys/input copies/received files removed. Cleanup also runs on assertion failure and deadline expiry. |

Application references are local: do not require equal database row IDs,
conversation UUIDs or contact UUIDs across A and B. Correlate peers by authenticated
public identities, messages by protocol references, and files by protocol transfer
IDs plus expected fixture identity. Do not require exact event counts or transient
states that can be coalesced. Events help correlate transitions; final domain
state and received bytes are the acceptance oracle.

## Waits and failure handling

Use monotonic deadlines and bounded event/state polling, with a maximum 64-item
page and 1 MiB result. Start polling at 100 ms and cap backoff at 1 second; avoid
fixed sleeps. Readiness waits have separate failure labels:

| Condition | Deadline |
|---|---|
| Application ready, per instance | 30 seconds |
| Whole private Tor network ready | 120 seconds |
| Client Tor/onion ready, per instance | 90 seconds |
| Public bio, contact request, acceptance/established, profile/avatar or message | 30 seconds per milestone |
| One file offer through verified completion | 60 seconds |
| One expected MCP approval | Resolve within 10 seconds; configured timeout 60 seconds |
| Whole scenario, including setup | 600 seconds |
| Cleanup, additional reserved budget | 20 seconds |

The whole-scenario deadline bounds cumulative waits. A required subsystem or
unsupported operation is a test failure, not a skip or fallback to mocks/public
Tor. A happy-path run injects no disconnect, crash, rejection or timeout, while
allowing the protocol's normal auxiliary connections/reconnections.

On failure, stop pending work, collect bounded sanitized diagnostics, then clean
up in `finally`. Escalate graceful shutdown to terminate/kill within cleanup's
budget. Keep only milestone labels, build/run IDs, correlation IDs, states,
relative timings, exit status and sanitized events/logs. Report fixture ID and
pass/fail for content comparisons without actual/expected payload dumps. Redact
credentials, onion sharing data, bios, text, filenames/paths in peer events and
file contents; disable raw traceback/state dumps and core files. Never retain
profile databases, vaults, Tor keys or received media as failure artifacts.

## Implementation dependencies and MCP review

The Phase 1 catalog alone cannot execute this scenario. The isolated test build
now supplies these dependencies through production authorization and domain
services, without registering new production tools:

1. Isolated bootstrap for the identity/profile/avatar/security preference setup,
   private Tor configuration and protected credential provisioning above.
2. Controller-isolated approval inspection/allow-once and bounded state/event
   inspection, using production domain state and approval transitions.
3. An authorized public-bio probe and inspection of the resulting pending-request
   bio. The current `profile` tool reads local cache only. A remote probe has
   network/privacy effects and must be treated as an approved operation; a test
   seam must call the existing workflow, without altering its wire behavior.
4. Basic authorized file offer, accept and bounded transfer inspection plus
   receiver integrity verification. Offers require `send_files`; reading received
   content requires `read_files`. Mutations and sensitive reads use the production
   approval manager and applicable identity/contact policies. Paths are restricted
   to isolated permitted roots; no arbitrary filesystem access or encoded media
   in MCP arguments/results. File offer acceptance must respect local preferences.

The owner's Phase 2 request brings the basic happy-path file coverage forward
from the deferred backlog. The implementation registers these operations only in
explicitly activated test builds. General production tool names/schemas and their
full capability mapping still require owner review under `agents.md` before
registration.
Resume, rejection, cancellation, corruption, restart/crash recovery, privacy
denials and the rest of the regression matrix remain separate scenarios.
