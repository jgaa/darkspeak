# DarkSpeak protocol v2

This document is the authoritative description of the current application
protocol. It describes the post-handshake encrypted stream. The old MD5/JSON
transport description is obsolete.

## Secure stream

The client/server handshake is version 1 and establishes authenticated
libsodium `crypto_secretstream_xchacha20poly1305` streams. After the handshake,
each direction has an independent stream state. Secretstream authentication
protects frame ordering and detects truncation, tampering, and invalid tags.

## Frame format

Each frame is written as two authenticated stream messages:

1. A 16-bit unsigned big-endian frame length.
2. The frame body.

The length is authenticated separately so the receiver can determine how many
bytes to read. It is not trusted until authentication succeeds.

The frame body is:

| Field | Size | Meaning |
| --- | ---: | --- |
| frame version | 1 byte | Always `2` |
| channel | 4 bytes | Big-endian channel identifier |
| frame id | 8 bytes | Big-endian, strictly increasing per direction |
| payload | remaining bytes | Control JSON or file bytes |

The frame body is at least 13 bytes and at most 65,535 bytes. Therefore the
maximum file payload is 65,522 bytes; the application currently uses 8 KiB
file chunks.

Channel `0` is the control channel. Non-zero channels carry file data. Channel
identifiers are allocated per direction and must not be reused while active.
An unknown channel is a protocol error and closes the connection.

Frame ids start at one and must increase strictly for every received frame,
regardless of channel. A duplicate, replayed, zero, or out-of-order id is a
protocol error and closes the connection.

## Control packets

Control payloads are compact JSON objects. Every packet contains:

```json
{"protocol":2,"type":"..."}
```

The `type` value selects the packet schema. The first packet on each encrypted
stream must be `Capabilities`:

```json
{
  "protocol": 2,
  "type": "Capabilities",
  "capabilities": ["frame-v2", "file-window-v1", "keepalive-v1"],
  "max-frame": 65535
}
```

Both peers send and validate this packet before other control packets are
accepted. Unsupported versions, missing fields, malformed JSON, unknown packet
types, and invalid packet fields are protocol errors and close the connection.

Current packet types are `Capabilities`, `KeepAlive`, `AddMe`, `Ack`, `Message`,
`IncomingFile`, `SetAvatar`, and `UserInfo`. Their existing application fields
remain unchanged; all are carried inside the versioned control envelope.

`KeepAlive` packets contain a random nonce and random padding. Each peer sends
them independently at a randomized interval between 90 and 240 seconds. They
are encrypted like all other control traffic and require no response. Tor's
own circuit maintenance is separate and does not replace application-stream
liveness traffic.

`Ack` packets identify the operation with `what` and its result with `status`.
Request ids identify the transport request that carried the packet. Application
handlers must validate ownership, authorization, state, and object ids before
changing local state. Repeated application requests are handled according to
the target object's current state; invalid state transitions are rejected.

## Scheduling and resource limits

Control traffic has priority over file offers and file data. The sender has a
bounded socket output queue of 256 KiB. File data is generated only when the
socket output queue has drained, and the scheduler rotates between active file
channels so one transfer cannot monopolize the connection.

At most two file transfers are active per contact connection. Additional
transfers remain queued. The application never enqueues an entire file.
Encrypted connections remain open while the underlying socket is connected;
keepalive traffic prevents an otherwise idle application stream from being
mistaken for a dead connection.

File payloads begin with a 16-byte header: an 8-byte big-endian chunk sequence
and an 8-byte big-endian byte offset. The receiver accepts only the next
contiguous sequence and offset. Duplicate chunks are harmless and are answered
with the current cumulative acknowledgement; gaps are rejected.

The receiver acknowledges chunks with a versioned control packet:

```json
{
  "protocol": 2,
  "type": "Ack",
  "what": "IncomingFile",
  "status": "ChunkAck",
  "data": "<file-id>",
  "sequence": "7",
  "offset": "65536",
  "final": "0"
}
```

The sender keeps eight chunks in flight, retransmits unacknowledged chunks
after two seconds, and fails the transfer after five retries. The sender does
not mark the transfer complete until the final cumulative acknowledgement is
received and the receiver has validated the final hash.

The file id is the transfer id. Offers carry immutable name, size, SHA-256 hash, and
file-id metadata. Resume is allowed only when the receiver has a matching
transfer metadata record and a partial file with the expected verified prefix;
otherwise the partial file is discarded and the transfer restarts at offset
zero. On disconnect, acknowledged sender offsets are preserved and active
transfers return to the offer/accept state for a verified resume.

## Error and close behavior

The connection closes on an unauthenticated length, invalid frame size, bad
frame version, non-monotonic frame id, malformed control packet, unsupported
control version, invalid capability declaration, unknown control type, or
unknown channel. No unauthenticated or malformed payload is dispatched to the
application layer.

## Testing requirements

Phase 4 tests cover frame bounds and versions, control envelope and capability
negotiation, frame-id ordering, malformed packets, unknown channels, channel
fairness, output queue bounds, authorization/state rejection, disconnects, and
control traffic while file transfers are active.
