# P2-002–004: privacy, recovery and deletion

All scenarios create two fresh isolated application-service processes, use real
private Tor/onion services, use separately authenticated controller catalogs and
production operator approval, and retain only sanitized reports. No AI, runtime
asset download, user profile or public Tor is involved. Prerequisite failures and
unexpected application exits are fatal; independent assertion failures continue,
until three consecutive checks fail. Each scenario has a 600-second deadline,
20-second cleanup budget and 630-second CTest deadline.

## P2-002 (`privacy_multi`)

Each process creates two independent identities, distinct signing keys and one
ordinary MCP key restricted to each identity. All four bios are private and carry
distinct fixture text. Probe each remote identity in both directions before
approval: the bio must remain empty. Public avatar reception is the authenticated
profile-exchange milestone; continue observing empty bios for at least three
seconds. A test-build wire observer counts only field-presence bits: require an
empty response, no outgoing bio content/hash and no forbidden frame before
approval. The observer runs after serialization/encryption and never retains
content or hashes; the receiver discarding forbidden content cannot hide a leak.
A pending outgoing request must also have no restricted bio. Accept each
pair separately, then require the correct restricted bio in both directions.
Send distinct text and a checksum-verified JPEG through each pair. Check contact,
profile, transfer ownership and ordinary-key identity boundaries. Restart both
instances and require both identities, signing keys and contact ownership to
persist. No private bio, text, filename, raw SDK exception or credential enters
retained artifacts.

## P2-003 (`resume_crash`, `receiver_crash`)

Establish one approved pair. Generate a deterministic 16 MiB JPEG attachment from
the checked-in NASA image plus reproducible trailing bytes inside the disposable
profile. Observe a strictly nonzero, incomplete incoming transfer, SIGSTOP then
SIGKILL the sender externally. Failure to interrupt before completion fails the
test; a transfer starting at zero cannot satisfy this prerequisite. Require a
failed incoming record with nonzero partial progress. Restart the same sender
profile with new MCP credentials and the same identity; wait for reconnection.
Invoke the normal incoming failed-file resume action through approved test MCP.
Require the same transfer ID to complete on both ends, no duplicate transfer
records, and full receiver SHA-256/size equality. Require observed nonzero
negotiated resume offsets at both ends. A transfer that fails after negotiation, or is rejected/cancelled, fails the
completion check immediately. The original failed state may persist briefly
while the remote peer processes resume admission; a harness regression test
checks that this expected state is allowed to advance.
A focused C++ test also delays receiver readiness, requires no sender start before
acknowledgement, rejects a stale-connection acknowledgement and checks the offset.
Verify messaging
after recovery. A separate `receiver_crash` case kills and restarts the receiver
instead, and requires its durable partial to survive that restart.

## P2-004 (`delete_contacts`, `delete_sender`)

Create two independent approved identity pairs. Start a nonzero incomplete transfer
on the first pair and pause its sender externally. Delete the receiving contact
while that transfer is active. Require removal of its contact, transfer and
conversation records, reject stale transfer access, and require sender failure
when it continues. Verify that the second identity can still receive a message.
Delete that idle contact, require empty local contacts/transfers/conversations,
restart the deleting instance and verify that none resurrects. A separate
`delete_sender` case freezes the receiver and deletes the sending contact during
the active transfer, then performs the same isolation and restart checks.

These cases cover transport loss/process crash and stale or cross-identity
operations. They do not claim malicious-wire fuzzing or corruption injection;
those require additional cases.
