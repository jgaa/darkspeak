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
