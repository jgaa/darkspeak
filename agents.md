This project is a secure instant messager using the Tor network as its network layer.

# Rules and guidelines

Cache the files locally (not checked into git) and either refresh or check the timestamp on the HTTP server when you refresh your Agents instructions, or checkout the agents repository https://github.com/jgaa/agents.git outside the project and keep it up to date before you re-read the files.

https://raw.githubusercontent.com/jgaa/agents/refs/heads/main/rules/tlv-agents.md
https://raw.githubusercontent.com/jgaa/agents/refs/heads/main/rules/tlv-agents-cpp.md

- You run as a seperate user than me. You don't have access to `/home/jgaa`
- The source code is in `/var/local/src/darkspeak/`
- If you lack write access to files or directory, ask me to update the permissions
- Do not compile (except for new tests you need to run). I will deal with that.
- The first priority is security. Use zero-trust architecture and assume that any peer is hostile.
- The second proirity is privacy. Do not leak any information that can be used to decude information about the user, the users location or anuthing else.
- Always be aware of a peers security preferences. Don't send or offer to send (in the UI) anything they don't accept.

## State

The project is being resurrected and improved. 

- **Protocol-level backward compatibility is a non-goal.** There are no supported legacy clients. Do not implement old-protocol negotiation/fallback, compatibility adapters, migration or legacy interoperability tests. Reject peers missing mandatory current protocol capabilities; use versioned formats only for current validation and safety. Ask the owner before any compatibility work. 

## Implementation worklists and scope control

- Use Markdown task checkboxes (`- [ ]` remaining, `- [x]` verified complete) for every actionable worklist item. **Check off completed items immediately**, citing the relevant test/commit when useful; never leave finished work as an indefinite "remaining" task. Keep a short, separate completed summary, not an ever-growing chronological diary.
- **Do not add, split into newly scoped tasks, reopen, or materially expand worklist items without explicit owner approval.** If new work seems necessary, explain the evidence, impact, estimated scope, and whether it blocks the current milestone; ask for approval **before** adding it. A test that reveals a defect within an already approved item can be fixed under that item; document it concisely rather than growing the project.
- Work on the current milestone only; defer unrelated improvements, speculative edge cases, refactors, extra compatibility, and downstream features. Security/correctness blockers must be reported promptly, not ignored, but they do not silently authorize expansion. Define finite acceptance criteria and stop when they pass. Do not move the release gate or invent new prerequisites after success without owner agreement.
- The currently approved generic file-transfer redesign is in `doc/file-transfer-coroutine-refactor.md`: one coroutine-owned attempt, not independent state-changing controllers. Read it only when doing generic transfer work. The prior checkpoint-implementation checklist was superseded; do not resurrect old coordination machinery to satisfy historical entries.
- In `doc/file-transfer-implementation.md`, keep the active gate and unchecked list near the top. Update evidence/status as work progresses; use Git history for implementation details. **No protocol-level legacy clients exist.** Checkpoint support is mandatory; unsupported peers fail safely, with no legacy transfer path.

## Useful investigation commands

```sh
rg --files -g '!build*'
rg -n 'secretstream|crypto_box|crypto_sign|DsCert|ADD_ONION|onion|channel|Ack' src doc
git -c safe.directory=/var/local/src/darkspeak log --oneline --all
cmake -S . -B /tmp/darkspeak-build -DCMAKE_BUILD_TYPE=Debug
```

Use `git -c safe.directory=/var/local/src/darkspeak ...` if Git reports dubious
ownership. Build directories belong in `/tmp` until the project has an agreed
build-directory convention.

## Test entry point maintenance

- `tests/run-all-tests.sh` is the entry point for building and running all unit,
  functional, GUI, integration and acceptance tests.
- Whenever tests are added, removed or renamed, review and maintain that script
  in the same change. Keep its prerequisites, build flags, Python dependency
  setup and execution behavior current; update `tests/README.md` as needed.
- Register every automated test with CTest, including future Python acceptance
  scenarios, and give it a bounded timeout. The script must build all required
  binaries and run the complete catalog without a fixed test-name filter.
- Keep ordinary runs isolated from real profiles and public Tor. Preserve
  sequential execution until parallel test isolation has been verified.

## Specification loading: minimize context

**Do not read all of `specs/` by default.** Read this index first, then load only the spec(s)/sections governing the task; use targeted `rg`/headings and line ranges for cross-references. Read additional specs only when an actual API, data-flow, security, or test dependency requires them. For small fixes, consult the relevant section rather than rereading entire large documents. Reuse already-read current content; refresh if its revision changed. Do not omit relevant security contracts just to save context. `specs/obsolete/` is historical, **never authoritative** unless specifically requested.

| Spec | Load when working on |
| --- | --- |
| `file_manager.md` | `IFileManager`, managed writable storage, temporary artifacts, reservations/quotas, writers/reads, crash recovery, persistence and namespace APIs. |
| `read_only_file_manager.md` | External read-only directory indexing, hashing/freshness, symlinks, exclusions, subscriptions and read-only backend tests. |
| `vfs.md` | Storage registry/identity assignments, mount topology and overlays, derived per-contact VFS, virtual IDs, browsing, filtering and catalog exports. |
| `ftp/fileshare_architecture.md` | **Orientation for cross-component FileShare work only**: ownership, integration boundaries and implementation order; not needed for isolated component fixes. |
| `ftp/fileshare_authorization.md` | FileShare permissions, group/contact policy precedence, upload acceptance, quotas, byte credits/ledger, reservations, refunds, coordinator and policy UI. |
| `transfer_queue.md` | Application-wide inbound/outbound transfer intent queue, feature adapters, persisted ordering, conversation-before-bulk admission, and verified-checkpoint priority preemption. Not generic transfer execution. |
| `transfer_queue_ui.md` | Transfers view, user-reorderable queue, priority and pause/resume controls, transfer progress and Settings → Transfers. |
| `ftp/fileshare_server.md` | Remote FileShare read-only service: UNIX-like virtual namespace, authorization, direct/nonrecursive LIST, explicitly approved recursive catalog, STAT, GET, privacy and limits. Implement only when the remote-server milestone is active. |
| `ftp/fileshare_protocol.md` | FileShare-specific v3 envelope controls, typed VirtualPath UTF8/ASCII escaping, result/status invariants, LIST/STAT/GET and cross-OS neutral wire presentation. Not the generic transfer protocol. |
| `ftp/fileshare_ui.md` | Normal FileShare administration UI: read-only/mutable storage dialogs, mounts, rules, groups, credit/upload controls, C++ models and test-MCP coverage. Not dashboard or wizard. |
| `mcp_and_testing.md` | MCP integration, production approvals/capabilities, test-only MCP, acceptance harness, authorization and MCP-facing model operations. For ordinary non-MCP tests, use this file only when its test harness contract matters. |
| `metrics.md` | Application-wide yahat metrics class, instrumentation, typed snapshots, optional OpenMetrics HTTP endpoint, metric privacy and tests. |
| `media_previews.md` | Image/video thumbnail creation, offers/rendering, preview policy and verification; not ordinary file-transfer changes. |
| `protocol.md` | Tor handshake, records, wire types, signatures, message/offer delivery, generic file-transfer checkpoint/ack/resume, wire-level validation. Also inspect `proto/darkspeak.proto` for actual wire declarations when changing protocol code. |

**Typical combinations:** FileShare mount editor → `ftp/fileshare_ui.md` + relevant `vfs.md`/`ftp/fileshare_authorization.md` sections; storage dialogs → `ftp/fileshare_ui.md` + relevant backend spec; FileShare credits → `ftp/fileshare_authorization.md` (+ protocol checkpoint section only if needed); FileShare server operations → `ftp/fileshare_server.md` + `ftp/fileshare_protocol.md` + targeted `vfs.md`/authorization sections; queue scheduling → `transfer_queue.md` (+ `transfer_queue_ui.md` only for UI); file-transfer wire logic → `protocol.md` + `proto/darkspeak.proto`; metrics → `metrics.md`; MCP test endpoints → relevant feature spec + `mcp_and_testing.md`. The FileShare dashboard and wizard are not yet specified here; do not infer contracts from unrelated specs. Read linked documents **selectively** rather than recursively loading every reference.

## Protocol specification:
 - Definition and rules: `specs/protocol.md`
 - Protofile: `proto/darkspeak.proto`

## Safety and protocol invariants

**All data derived from a peer is untrusted, including authenticated and
approved peers. No peer-controlled value may determine an allocation,
loop count, persistent-state count, filesystem path, queue size,
expensive operation, or state transition without first passing an
explicit local bound and authorization check. Parsing and validation
failures terminate the connection before application dispatch.**

- Be generous with trace and debug log statements so troubleshooting is simple. 
- Never log private keys, sealed payloads, or plaintext message/file contents.
- Never trust a peer-provided length, offset, chunk count, filename, or channel
  without bounds and checked arithmetic.
- Verify the claimed public key/signature before accepting a peer identity or
  enabling the encrypted stream.
- Keep control traffic independently schedulable and bounded while file data is
  in flight.
- Do not use assertions as input validation in network-facing code.
- Any new wire or database format must have an explicit version and regression
  fixtures. No migration or compatibility layer is required.
- Use Tor v3 addresses only. 
- When presenting `SymlinkPolicy::Always` in the UI, show the option and its active state in red with a clear warning that it can expose individual regular files outside the configured storage boundary; external directory traversal is forbidden. Show the assignment-level external-file permission with the same red warning.

