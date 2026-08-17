# Agent handoff notes

## General rules and info

- You run as a seperate user than me. You don't have access to `/home/jgaa`
- The source code is in `/var/local/src/darkspeak/`
- If you lack write access to files or directory, ask me to update the permissions
- Use modern C++20. Avoid old style "C" strings and other primitives. Use views and spans
- Do not compile (except for new tests you need to run). I will deal with that.
- Don't make assumptions about my preferences. Ask.
- The first priority is security. Use zero-trust architecture and assume that any peer is hostile.
- The second proirity is privacy. Do not leak any information that can be used to decude information about the user, the users location or anuthing else.
- Always be aware of a peers security preferences. Don't send or offer to send (in the UI) anything they don't accept.

## State

The project is being resurrected and improved. 

- No backwards compatibility is expected. Treat it as a new project for now. 

## Repository orientation

- Working tree: `/var/local/src/darkspeak` (the environment may display the
  same checkout as `/home/codex/src/darkspeak`).
- Main targets: `crypto`, `tor`, `prot`, `core`, `models`, and `darkspeak`.
- QML resources live in `src/app`; public headers are under each library's
  `include/ds` directory.
- Persistent data is SQLite; `database/master.db` is reference material only.
  The resurrected application may use a fresh schema and disposable database.

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

## Protocol specification:
 - Definition and rules: `doc/protocol.md`
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
