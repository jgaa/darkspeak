# P2-005/006: transfer decisions and corrupted resume

Both scenarios use two new isolated application instances, one identity per
instance, real private Tor, normal domain services and exact-argument operator
approval. Each has the existing 600-second scenario deadline, 20-second cleanup
budget and 630-second CTest deadline. Retained diagnostics contain fixed check
labels, outcomes and bounded metadata; no credentials, SQL contents, paths or
file/profile/message bytes are retained.

## P2-005 (`file_decisions`)

Reject offers in both directions before acceptance. Both records must become
rejected, transfer zero bytes and allocate no incoming storage. Cancel a transfer
with nonzero progress from its receiving side, then another from its sending
side. Both ends must reach cancelled state. Incoming content must be unpublished
and its partial must be removed. Messaging must remain usable. Restart both
instances and require rejected/cancelled states to persist, with no publication
or partial-file resurrection.

Test-only `file_reject` and `file_cancel_incoming` require `read_files` and its
contact policy; `file_cancel_outgoing` requires `send_files` and its contact
policy. These invoke the GUI's normal file actions, reject wrong directions or
terminal states, and remain unavailable outside explicit isolated test bootstrap.

## P2-006 (`corrupt_partial`)

Interrupt a real transfer after nonzero durable progress and stop its receiver.
An external Python utility reads only that isolated instance's transfer metadata,
then flips one byte in the matching staging file without changing its size. The
utility requires a stopped application, the test profile marker and owned private
nonsymlink directories; the leaf must be a singly linked owned regular file.
Focused harness tests verify confinement and refusal for unsafe inputs.

Restart and reconnect, then attempt ordinary resume using the original immutable
file metadata. The corrupted file must never become published/readable. A backend
may reject the prefix immediately or reject the complete checksum after resumed
transfer. Observe for a bounded window, require failed state, and verify messaging
still works. Restart again and require the corrupted file to remain unpublished.
Reconnect and complete a new valid file transfer with full receiver SHA-256
verification, proving subsequent transfers remain usable.

The authenticated controller's `controller_storage` inspects normal managed
storage metadata without opening a writer or reading file contents. Its pending
inspection is bounded to one page of 64 entries; additional pages fail explicitly.
It returns only publication/pending status, transfer state and byte count.
