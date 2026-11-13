# Isolated MCP regression infrastructure

P2-001 runs two independent headless application instances with DarkSpeak's real
`Manager`, `DsEngine`, Qt models, encrypted managed storage, protocol and Tor
services. It does not load the QML window. Focused rendering/binding checks remain
in `mcp_ui`; this suite asserts protocol and persistence behavior. No AI service,
public Tor, existing application profile or user's Tor daemon is used.

## Setup and execution

For the complete repository suite, use [tests/run-all-tests.sh](../run-all-tests.sh).
The commands below are for building and running the MCP checks on their own.

Linux, Python 3.12+, normal C++/Qt/MCP dependencies, `tor` and `tor-gencert` are
required. Tested locally with Qt 6.10.2, Python 3.14.4 and Tor 0.4.9.11. CMake
fetches Chutney at the root CMakeLists.txt pin. Only its local configuration
launcher is used, without SSH/RPyC servers. Dependency preparation requires
internet access; execution uses checked-in assets and a private Tor network.

```sh
python3 -m venv /tmp/darkspeak-mcp-venv
/tmp/darkspeak-mcp-venv/bin/python -m pip install -r tests/mcp/requirements.txt
cmake -S . -B /tmp/darkspeak-mcp-tests \
  -DBUILD_TESTING=ON -DDARKSPEAK_WITH_MCP=ON -DDARKSPEAK_WITH_TEST_MCP=ON \
  -DPython3_EXECUTABLE=/tmp/darkspeak-mcp-venv/bin/python
cmake --build /tmp/darkspeak-mcp-tests --parallel 3 --target \
  mcp_test_instance mcp_policy_test mcp_service_test mcp_ui_test managed_storage_test
```

These targets build tests and their dependencies, without building the graphical
application executable. Run the first scenario, or all implemented checks:

```sh
ctest --test-dir /tmp/darkspeak-mcp-tests -R '^mcp_peer_happy_path$' --output-on-failure
DARKSPEAK_MCP_TEST_PYTHON=/tmp/darkspeak-mcp-venv/bin/python \
  ctest --test-dir /tmp/darkspeak-mcp-tests --parallel 1 \
  -R '^(mcp_.*|managed_storage)$' --output-on-failure
```

For manual investigation, the same Python orchestration prints each safe
milestone as it completes:

```sh
/tmp/darkspeak-mcp-venv/bin/python tests/mcp/run_scenario.py \
  --instance /tmp/darkspeak-mcp-tests/bin/mcp_test_instance \
  --chutney /tmp/darkspeak-mcp-tests/_deps/chutney-src \
  --artifacts /tmp/darkspeak-mcp-reports
```

The pytest entry point is `test_peer_happy_path.py`. Set
`DARKSPEAK_TEST_INSTANCE`, `DARKSPEAK_TEST_CHUTNEY` and optionally
`DARKSPEAK_TEST_ARTIFACTS`. Missing infrastructure fails instead of skipping.
`test_harness.py` needs neither Tor nor a compiled instance. `test_bootstrap.py`
uses `DARKSPEAK_TEST_INSTANCE` to check unsafe-root rejection, Tor-independent
readiness, and identity/avatar preservation across a real process restart.

## Isolation and controller

The test executable and test-enabled graphical application accept exactly:

```text
--activate-mcp-test --mcp-test-profile /tmp/ds-mcp-<run>/<peer>
```

Before Qt/settings initialization, bootstrap requires real owner-only run/profile
directories, a versioned owner-only marker and protected configuration. It rejects
symlinks, shared roots, hard-linked configuration, production roots and missing
activation. HOME/XDG paths, vault, databases, downloads and input copies remain
inside that root. A process lock excludes concurrent use. Core dumps are disabled;
parent death terminates instances.

`bootstrap.json` version 1 contains A/B `label`, four distinct ports (`mcp_port`,
`controller_port`, `tor_control_port`, `tor_socks_port`) and a random 64-character
`vault_passphrase`. The protected `.darkspeak-test-profile-v1` marker contains
`darkspeak-mcp-test-v1` plus newline. Protected `provision.json` version 1 delivers
`identity`, `ordinary_secret`, `controller_secret` and the two MCP ports; the
harness consumes and deletes it immediately. No credentials enter arguments,
URLs or logs. Restart reuses the profile/vault and provisions fresh MCP keys.

The controller has a separate authenticated loopback listener and frozen catalog:

| Tool | Purpose |
|---|---|
| `controller_state` | Bounded readiness, identity/avatar/contact/transfer metadata and sanitized diagnostics |
| `controller_pending` | Complete immutable approval arguments; sensitive, never log |
| `controller_decide` | Production approval manager decisions/scopes for any key in this instance |
| `controller_online` | Start normal Tor application services |
| `controller_hash` | Read a completed incoming fixture through managed storage; return only SHA-256 and size, at most 1 MiB |
| `controller_shutdown` | Stop networking and drain storage |

The explicitly activated controller is the simulated operator. It cannot override
key/capability/contact denials or administer keys/policies. Ordinary credentials
cannot authenticate here; controller credentials cannot authenticate on ordinary
listeners or other instances. Decisions retain normal validation, expiry and
execution through `Service`.

Only explicitly activated test instances add these ordinary operations, all using
production authorization, approvals, durable operation handling and domain services:

| Tool | Capability/contact permission | Behavior |
|---|---|---|
| `bio_probe` | `read_profiles` | Approved outbound public-bio probe using `sharing_id` |
| `bio_preview` | `read_profiles` | Approved read of the local probe result, bound to `sharing_id` |
| `file_offer` | `send_files` | Import/offer a provisioned `asset` to `contact` |
| `file_accept` | `read_files` | Accept incoming `transfer` for `contact`, subject to local policy |
| `files` | `read_files` | Approved sensitive inspection, at most 64 records for `contact` |

Each requires `identity` and `idempotency_key`. Assets are restricted to
`earth.jpg`, `apollo11.jpg`, `sos.ogg`; transfer references are 32 lowercase hex
characters. There are no caller paths, SQL statements or media bytes in MCP.
All extensions/controller/bootstrap code is excluded when the test flag is OFF.
These are not new production tools; general production schemas still require
owner review.

## Private Tor, failures and cleanup

Chutney configures four authorities, three non-exit relays and two clients, with
separate state and SAFECOOKIE controls. All listeners are loopback. No public
authorities, fallbacks, bridges or exits are used. Clients restrict reachable
addresses to loopback; Tor forbids that setting on relays, whose authority and
consensus set is exclusively local. Endpoint allocation avoids other endpoints
in the same run. Bind failure is fatal, without fallback to another daemon.

Application readiness includes storage and is independent of Tor. Private-network
readiness requires all authorities, both clients and at least five ready servers;
unused spare relays do not gate it. Onion-service and contact readiness are
separate milestones. Waits use state predicates and monotonic deadlines; MCP
calls are paced below production limits. See the [scenario](scenarios/001-two-peer-happy-path.md)
for limits: 600 seconds overall plus bounded diagnostics/cleanup, 630 in CTest.
Sequential execution is deliberate.

Unavailable prerequisites, failed isolation/authentication, process exits and
unexpected approvals abort immediately. Recoverable assertion/transfer failures
are recorded and independent checks continue, including the opposite direction.
Three consecutive failures abort; success resets the streak. A failed/cancelled
call resolves only its own exact pending approval before continuing.

Each run retains a versioned `p2-001-<random>.json` report with safe check labels,
outcomes, codes, durations, exit status and a bounded state projection. A bounded
in-memory diagnostic ring keeps fixed kinds/levels and recognized numeric storage
codes. Free-form application/Qt/Tor logs are discarded. No raw local capture is
enabled. Credentials, sharing information, text, bios, filenames/paths, profiles,
vaults, Tor keys and received media are excluded from reports and tracebacks.

Cleanup attempts graceful shutdown, then terminates/kills and reaps owned process
groups and removes the private root, including on failure. Parent-death guards
and lifetime ceilings protect against a lost harness. Only JSON reports survive.
The GitHub workflow uploads those reports for seven days, uses source path filters
and runs sequentially on master. Remote workflow execution has not been verified
locally. The wider Phase 2 matrix remains future work.
