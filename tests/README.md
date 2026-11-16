# Running all tests

From any working directory, invoke the repository's test entry point:

```sh
./tests/run-all-tests.sh
```

It prepares a private Python virtualenv from the pinned
`mcp/requirements.txt`, configures all test features, builds the application and
test binaries, and runs the complete CTest catalog sequentially. This includes
unit, file-manager, functional, Qt Quick, QML smoke, MCP and private-Tor acceptance
tests. Future acceptance tests are included when registered with CTest. The
legacy optional `tor_integration` registration is enabled too; its current
implementation reruns the crypto handshake test, while the real peer scenario
uses the isolated private Tor network.

The existing file-manager test registration also checks common entry/hash/readiness
and capability metadata, visibility/read refusal for updating entries, and Linux
source-name encoding (raw invalid UTF-8, escaped-percent collisions, case and
normalization distinctions, and maximum expansion). These need no extra flags or
dependencies.

The preferences tests include desktop notification redaction combinations and
the global enable setting; managed-storage tests also cover identity preference
defaults, isolation and persistence. They use the existing test registrations
and require no additional runner flags or dependencies.

Image-preview coverage includes the `media_preview` codec test, signed attachment
validation in `protocol_v3`, private-cache and deletion checks in
`managed_storage`, a Qt Quick offer-card binding check, and the isolated Tor
`mcp_peer_image_previews` / `mcp_peer_image_previews_disabled` scenarios. They
use existing Qt image plugins and checked-in assets; no video helper is needed.

The managed-storage suite also exercises reconnection on queued messages and
files with a fake transport: offline identities, disabled contact auto-connect,
connection-attempt coalescing, manual cancellation, blocks and inbound-only work.
It does not contact Tor or a peer for these checks.

Transfer lifecycle coverage also checks persisted checkpoint mode, typed pauses,
partial-content retention, cancellation, terminal failure recovery, stale-attempt
isolation and recovery when the application pause mirror fails. Scoped
transfer-ID collision tests use separate contacts without network access.
`database_recovery` checks real SQLite reopen and idempotent restart recovery of
checkpoint, legacy and credit-wait states. Numeric states and the versioned
application mirror schema are frozen in `fixtures/transfer-lifecycle-v1.json`.
The managed-storage suite checks that asynchronous resume validation takes a
concurrency slot before filesystem work. UI component tests check pause reasons
and resume visibility without opening real profiles or Tor connections.
These extend existing CTest registrations and require no runner changes.

Checkpoint recovery tests additionally enforce exact retained references (including
final/pause flags), EOF-only selection for completed transfers after restart, and
empty-file final references. `transfer_checkpoints` verifies that attempt preflight
does not mutate progress and that commit rechecks terminal failures.
`managed_storage` verifies that unretained checkpoints and stale sequence choices
cannot invalidate partial bytes, and that superseded validation cannot install or
restore an old runtime attempt. These use the existing bounded CTest registrations;
`run-all-tests.sh` needs no new targets, flags or dependencies.

The managed-storage checkpoint matrix also runs real authenticated encrypted
loopback peers through the application contact queues: 64 KiB, 1 MiB and 16 MiB
intervals, empty EOF, signed primary/preview manifests, withheld checkpoints,
stale-attempt ACK rejection and advisory pauses without progress advancement.
The matrix also exercises nonterminal checkpoint resume after a lost mutual
receipt, bounded fallback after receiver-prefix corruption, source absence,
locally proved source mismatch and conservative backend revalidation refusal. Rewind
coverage rechecks accepted source prefixes before their boundaries are sent, including
short pause prefixes inside ordinary chunks. Known checkpoint mismatch, unsupported
version and remote source-failure cancellations retain typed outcomes without producing
local corruption/refund proof.
Malformed retained candidates fail closed before legacy application dispatch;
duplicate exact readiness coalesces while its journal commit is pending, and
late offer rejection cannot rewrite a completed outcome. Shared contact work
budgets remain occupied until cancelled asynchronous work actually returns.
The format-two progress journal fixture covers direction-scoped key bytes and a
monotonic verified prefix through resume rewinds and checkpoint retention. Empty
format-one catalogs can be reinitialized transactionally; populated catalogs
remain untouched and unsupported, including across an injected commit failure. Corrupt
catalog scalar values, attempts, sequences and completion claims fail closed on load.
Production capability advertisement stays disabled pending reconnect/restart and
terminal replay coverage. `protocol_v3` rejects unilateral checkpoint capability
before application dispatch. The existing managed-storage CTest timeout is 180
seconds for this expanded matrix; the full runner already honors it and needs no
additional dependencies or flags. Set `DS_TEST_TRACE=1` for fixture trace logging.

Install the normal C++/Qt/MCP dependencies, Python 3.12+ with venv/pip, `tor` and
`tor-gencert` and `ffmpeg` first. Video decoding tests need a Linux kernel with
Landlock and seccomp support. The script does not install system packages. Dependency
preparation can use the internet; peer tests use local Tor and checked-in assets.

Video coverage includes actual FFmpeg frame extraction, a test subprocess that
verifies descriptor/environment isolation, filesystem/network restrictions,
output and time limits, and cancellation. `video_preview_support` checks native
startup advice and per-profile “Don’t show again” persistence; Qt Quick checks
the dialog controls. `mcp_peer_video_previews` and
`mcp_peer_video_previews_disabled` verify real peer video preview transfers and
opt-out with the small generated FFV1 fixture in `fixtures/media-preview-v1/`.

```sh
./tests/run-all-tests.sh --build-dir /tmp/darkspeak-tests --jobs 3
./tests/run-all-tests.sh --python /path/to/python3 -- -DCMAKE_PREFIX_PATH=/opt/Qt
./tests/run-all-tests.sh --dry-run
```

The default build directory is `/tmp/darkspeak-all-tests-$UID`. Builds and the
virtualenv are retained for reuse. Test HOME/XDG directories are temporary and
removed on exit; acceptance tests additionally manage their own profiles and
process cleanup. Core dumps are disabled. Scenario reports remain in
`<build-dir>/tests/mcp-artifacts/`; only sanitized reports should be shared.
CTest logs and GUI smoke profiles remain private in the build directory.

Configure/build errors stop immediately. CTest continues through independent
tests and returns failure if any test fails. Each test's own timeout takes
precedence over the 180-second default. There is no test-name filter or silent
fallback when acceptance infrastructure is unavailable. See
[MCP infrastructure](mcp/README.md) for individual scenarios and failure handling.

Whenever tests are added, removed or renamed, maintain this script and the CTest
registrations in the same change, as required by [agents.md](../agents.md).

The sequential CTest catalog also includes private-Tor bio privacy/multiple
identity, interrupted-file resume and contact-deletion scenarios. The top-level
runner was reviewed for these additions: its unfiltered CTest invocation discovers
all five, per-test deadlines override the default, and no extra dependencies or
build flags are needed. See [scenario definitions](mcp/scenarios/002-error-and-isolation.md).

`database_recovery` checks both transfer directions across every persisted state,
including idempotent restart recovery and preservation of nonzero offsets. It uses
the existing Qt/Core test dependencies and is discovered by the same full runner.

The managed-storage suite also checks that a resumed sender emits no chunks
until receiver readiness, preserves the negotiated offset and becomes resumable
on disconnect. Readiness is bounded to two pending transfers per contact and
30 seconds; no additional dependency or test-runner option is needed.

The full runner also discovers `mcp_peer_file_decisions` and
`mcp_peer_corrupt_partial`. Both use the same build flags and pinned dependencies;
fault setup is restricted to stopped private test profiles. See
[transfer-error scenarios](mcp/scenarios/003-file-decisions-and-corruption.md).


`readonly_filemanager` covers the external read-only backend, raw Linux names,
startup/restart reconciliation, partial-scan safety, Cached/Rehash/Snapshot
preparation, staged resume/expiry, resource quotas, subscription pins, opaque
requester-bound cursors, cross-backend aliases and exclusion revocation. The
bind-mount case uses `unshare` when private user/mount namespaces are available;
otherwise that subcase reports a skip. No additional Qt module is needed.
`managed_storage` also checks mandatory DarkSpeak exclusions and actual sender
completion/failure, including a resumed prefix and tampering that restores mtime.

The full runner discovers `readonly_million_entry_paging` without a test-name
filter. That scale test reports CTest skip code 77 unless
`DSFM_RUN_MILLION_ENTRY_TEST=1` is set and at least 6 GiB of temporary storage is
available. When enabled it creates and indexes one million real files using finite
entry/catalog/page budgets, checks complete keyset paging and UI responsiveness,
and has an explicit 1800-second timeout. The ordinary read-only suite performs the
same paging checks with 1,200 real files. Enable the larger run with:

```sh
DSFM_RUN_MILLION_ENTRY_TEST=1 tests/run-all-tests.sh
```

`vfs` covers identity/mount composition, persistent opaque IDs and format fixtures,
subtree confinement, mount-scoped authorization, synthetic pagination, filtered
subscriptions, cross-mount alias exclusions, revocation, writable delegation and
bounded catalog exports/validation. It runs against real FileManager backends
with a 120-second CTest deadline and no extra test-runner flags.
`managed_storage` also checks Home-pane filesystem-conflict notifications and
catalog payload completion/revocation through the existing outgoing transfer
channel. See `src/vfs/README.md` for the standalone test command.

The `vfs` target also covers persistent storage assignments and withdrawal,
identity/group/contact overlays, permission precedence/group conflicts,
nonrecursive parents and navigation-only reopening, bounded empty-page
continuations, and assignment-gated external-file symlinks. These cases are part
of the existing CTest target and require no additional runner dependency.

`storage_pool` exercises shared filesystem admission, independent usage classes,
namespace identity checks, materialized/outstanding accounting, retained claims
across restart, per-authority owner locking and independent concurrent profile authorities. It runs sequentially with temporary
private journals and no real profiles or network access.

`temporary_artifacts` covers private native publication, immutable finalized reads,
owner-bound resume leases, revocation, expiry with open handles, class isolation,
read-only snapshots sharing the pool, and process death during creation,
publication and deletion. `transfer_checkpoints` covers the independent durable
progress journal, sender-selected retained checkpoints, final/empty confirmation,
late duplicates, stale attempts (including delayed pause writes), interval bounds, and withheld-checkpoint fences.
`managed_storage` additionally exercises the sender and receiver channel fences,
ordinary ACKs without durable progress or per-ACK SQL updates, incorrect receiver
digests without source-integrity refunds, and final/empty-file confirmation before
publication and final acknowledgement. It also exchanges a complete payload
between real sender/receiver channels, exercises clipped chunks after a short
paused prefix, and retries the final chunk after publication.
Both use isolated private SQLite journals and process-kill fault injection.
