# DarkSpeak protocol v3

This document is the authoritative description of the current
application protocol. It describes the authenticated ephemeral
key-exchange handshake and the post-handshake encrypted protobuf stream.
The previous JSON/MD5 transport and protocol-v2 frame format are
obsolete.

The protocol is designed for an adversarial environment. Every network
peer must be treated as potentially malicious even when its
cryptographic identity is valid and the peer has been approved as a
contact.

**All data derived from a peer is untrusted, including authenticated and
approved peers. No peer-controlled value may determine an allocation,
loop count, persistent-state count, filesystem path, queue size,
expensive operation, or state transition without first passing an
explicit local bound and authorization check. Parsing and validation
failures terminate the connection before application dispatch.**

The application protocol is independent of the privacy transport.
DarkSpeak currently uses Tor, but the protocol requires only a reliable,
ordered, bidirectional byte stream. Tor-specific addressing, SOCKS
operation, circuit management, isolation, errors, and lifecycle state
belong to the transport backend and must not leak into application
framing, authentication, authorization, or message semantics. Future
privacy-strengthening transports may provide the same byte-stream
abstraction.

## Secure stream

Handshake version 2 is a fixed binary, three-message exchange and is not
protobuf encoded. It uses fresh ephemeral X25519 key pairs for every
connection, authenticates those ephemeral keys with the peers' long-term
Ed25519 signing identities, binds both directions to one handshake
transcript, and derives independent directional session keys with
libsodium `crypto_kx`. The ephemeral private keys are erased after
session-key derivation.

The resulting session keys initialize one
`crypto_secretstream_xchacha20poly1305` stream in each direction. Each
direction has an independent key and header. Secretstream authentication
protects post-handshake records against tampering, removal, duplication,
reordering, and modification. A later compromise of a long-term Ed25519
identity private key does not reveal session keys from previously
recorded handshakes, provided the ephemeral private keys and derived
session keys were erased as required.

Long-term signing keys and ephemeral key-exchange keys are distinct. The
identity certificate contains the Ed25519 signing identity. X25519 key
pairs used by `crypto_kx` are generated fresh for each connection and
are never derived from or reused as the long-term identity key.

The handshake authenticates a cryptographic endpoint identity.
Application code must not assume that this endpoint key will permanently
be identical to a human/user identity. This separation allows a future
user identity to authorize multiple independently revocable device
identities without copying one long-term private key to every device.

### Handshake transcript and domain separation

All handshake signatures and key-confirmation authenticators use
deterministic byte serialization exactly as specified below; they do not
authenticate language-native structures. Multi-byte integers are encoded
big endian. In the definitions below, `||` means exact byte concatenation
with no separator, terminator, or implicit length prefix. `ASCII("...")`
means exactly the listed ASCII bytes without a terminating NUL, and `U8(2)`
means the single byte value `0x02`.

The following ASCII domain-separation strings, without a terminating
NUL, are used for the corresponding signature or key-confirmation input:

``` text
DarkSpeak handshake client hello v2
DarkSpeak handshake server hello v2
DarkSpeak handshake client finish v2
DarkSpeak handshake client key confirmation v2
```

`ClientHelloBytes` and `ServerHelloUnsignedBytes` mean the exact fixed
binary wire bytes of those structures excluding their signature fields.
`ServerHelloBytes` includes the server signature. The transcript hash is
SHA-256 over the exact concatenation specified for that stage.

The client is expected to know the server's long-term Ed25519 signing
public key before connecting. The client includes that expected server
identity in its first signature so a signed `ClientHello` cannot be
transplanted to a different intended server identity.

### Handshake messages

For every connection the client generates:

-   a fresh X25519 `crypto_kx` ephemeral key pair;
-   a fresh 32-byte cryptographically random client nonce.

It sends one fixed-size `ClientHello`:

  ------------------------------------------------------------------------
  Field                                         Size Meaning
  --------------------- ---------------------------- ---------------------
  handshake version                           1 byte Always `2`

  client ephemeral                          32 bytes Fresh X25519
  public key                                         `crypto_kx` public
                                                     key

  client nonce                              32 bytes Fresh
                                                     cryptographically
                                                     random nonce

  client signing public                     32 bytes Long-term Ed25519
  key                                                endpoint identity

  signature                                 64 bytes Ed25519 signature
                                                     described below
  ------------------------------------------------------------------------

The client signature input is exactly:

``` text
ASCII("DarkSpeak handshake client hello v2") ||
U8(2) ||
expected_server_signing_public_key ||
client_ephemeral_public_key ||
client_nonce ||
client_signing_public_key
```

The complete `ClientHello` is exactly 161 bytes.

After receiving it, the server:

1.  reads exactly 161 bytes within the handshake deadline;
2.  validates the version and fixed field sizes;
3.  validates the announced signing-key encoding and treats the
    32-byte ephemeral public key only as input to libsodium key exchange;
4.  verifies the client signature;
5.  applies global handshake/resource limits and any identity blacklist
    or rejection policy;
6.  generates a fresh X25519 `crypto_kx` ephemeral key pair and fresh
    32-byte server nonce;
7.  derives directional session keys using
    `crypto_kx_server_session_keys()` with the two ephemeral key pairs;
    and
8.  initializes its server-to-client secretstream push state with the
    derived server transmit key, producing a fresh secretstream header.

A valid signature authenticates the announced endpoint identity, but the
server must not enter `ENCRYPTED_UNAPPROVED`, create application-visible
persistent state, or dispatch application traffic until `ClientFinish`
has been verified, including explicit client session-key confirmation.
A replayed `ClientHello` therefore cannot recreate an old session and
cannot complete a new one without deriving the client-to-server session
key from the corresponding client ephemeral private key.

The server sends one fixed-size `ServerHello`:

  ------------------------------------------------------------------------
  Field                                         Size Meaning
  --------------------- ---------------------------- ---------------------
  handshake version                           1 byte Always `2`

  server ephemeral                          32 bytes Fresh X25519
  public key                                         `crypto_kx` public
                                                     key

  server nonce                              32 bytes Fresh
                                                     cryptographically
                                                     random nonce

  server stream header                      24 bytes Header for
                                                     server-to-client
                                                     secretstream

  signature                                 64 bytes Ed25519 transcript
                                                     signature
  ------------------------------------------------------------------------

The server signature input is exactly:

``` text
ASCII("DarkSpeak handshake server hello v2") ||
SHA-256(ClientHelloBytes) ||
U8(2) ||
server_ephemeral_public_key ||
server_nonce ||
server_stream_header
```

The complete `ServerHello` is exactly 153 bytes.

After receiving `ServerHello`, the client:

1.  reads exactly 153 bytes within the handshake deadline;
2.  validates the version and fixed field sizes;
3.  verifies the server signature using the expected server signing
    public key;
4.  derives directional session keys using
    `crypto_kx_client_session_keys()` with the two ephemeral key pairs;
5.  verifies that session-key derivation succeeds;
6.  initializes its inbound server-to-client secretstream pull state
    with the derived client receive key and the received server stream
    header; and
7.  initializes its outbound client-to-server secretstream push state
    with the derived client transmit key, producing a fresh client
    stream header.

The client then sends one fixed-size `ClientFinish`:

  ------------------------------------------------------------------------
  Field                                         Size Meaning
  --------------------- ---------------------------- ---------------------
  handshake version                           1 byte Always `2`

  client stream header                      24 bytes Header for
                                                     client-to-server
                                                     secretstream

  signature                                 64 bytes Ed25519
                                                     full-transcript
                                                     signature

  key confirmation                          32 bytes `crypto_auth`
                                                     authenticator using
                                                     `client_tx`
  ------------------------------------------------------------------------

The client signature input is exactly:

``` text
ASCII("DarkSpeak handshake client finish v2") ||
SHA-256(ClientHelloBytes || ServerHelloBytes) ||
U8(2) ||
client_stream_header
```

`ClientFinish` also carries explicit client session-key confirmation. The
confirmation value is computed with libsodium `crypto_auth()` using the
derived client-to-server session key (`client_tx`) as its 32-byte key.
Its input is exactly:

``` text
ASCII("DarkSpeak handshake client key confirmation v2") ||
SHA-256(ClientHelloBytes || ServerHelloBytes) ||
U8(2) ||
client_stream_header
```

The server verifies the confirmation with `crypto_auth_verify()` using
its corresponding `server_rx` key. This proves that the client completing
the handshake derived the same client-to-server session key and therefore
possesses the ephemeral secret required for this exchange.

The complete `ClientFinish` is exactly 121 bytes.

After receiving `ClientFinish`, the server:

1.  reads exactly 121 bytes within the handshake deadline;
2.  validates the version and fixed field sizes;
3.  verifies the client signature with the already authenticated client
    signing public key;
4.  verifies the client key-confirmation authenticator with
    `crypto_auth_verify()` using `server_rx`;
5.  initializes its inbound client-to-server secretstream pull state
    with the derived server receive key and the received client stream
    header; and
6.  only then enters `ENCRYPTED_UNAPPROVED`.

The resulting directional mapping is:

  -----------------------------------------------------------------------
  Direction         Client key        Server key        Secretstream
  ----------------- ----------------- ----------------- -----------------
  client -\> server `client_tx`       `server_rx`       client push,
                                                        server pull

  server -\> client `client_rx`       `server_tx`       server push,
                                                        client pull
  -----------------------------------------------------------------------

The implementation must verify that the directional keys have the size
required by `crypto_secretstream_xchacha20poly1305`. If a future
libsodium API changes these sizes or they are otherwise incompatible,
the handshake fails rather than truncating, padding, or transforming
keys implicitly.

The client securely erases its ephemeral X25519 private key immediately
after successful session-key derivation and initialization of the
required secretstream states; it retains only the session material still
needed to complete `ClientFinish` and the live secretstream states. The
server securely erases its ephemeral X25519 private key after successful
session-key derivation and, no later than successful verification of
`ClientFinish`, erases temporary session-key material no longer required.
Both sides erase temporary transcript/MAC material and standalone derived
keys as soon as they are no longer needed. Long-lived secretstream state
remains only for the lifetime of the connection.

The fixed handshake messages are exchanged before transport-record
framing is enabled. After `ClientFinish` has been verified by the
server, including explicit client key confirmation, all further bytes
use the transport-record format below. The first application message in
each direction is `Capabilities`.

The three-message handshake provides explicit client session-key
confirmation and implicit server session-key confirmation. The client
has authenticated the server's long-term identity and signed ephemeral
key in `ServerHello`, but does not consider the server's possession of
the corresponding ephemeral/session secret confirmed until it
successfully authenticates the first encrypted server-to-client record.
That first record must be `Capabilities`. Failure to authenticate it
terminates the connection; no fourth handshake message is used.

Unsupported versions, invalid fixed sizes, failed or rejected ephemeral
key exchange, invalid signatures, failed client key confirmation, invalid
stream initialization, transcript mismatch, failure of implicit server
key confirmation, or handshake timeout close the connection. Handshake material, private keys, derived session keys,
signatures over secret material, sealed/plaintext application data, and
plaintext application payloads must never be logged.

### Handshake resource and timing limits

Handshake parsing must not allocate based on peer-controlled sizes: all
three messages are fixed size. The implementation must enforce a bounded
handshake deadline and global limits on simultaneous incomplete
handshakes. Per-identity limits are not sufficient because an attacker
can generate identities freely.

The implementation must also bound expensive cryptographic work
globally. Failure paths must release handshake state promptly and must
not create persistent application state.

## Transport record

The encrypted stream is transported as a sequence of records:

``` text
4-byte unsigned ciphertext length, big endian
ciphertext of one TransportEnvelope
```

The length is the ciphertext length, including secretstream
authentication overhead. It is an untrusted framing value and is not
authenticated separately. The receiver must validate it before
allocation or waiting for the record body. A length greater than
`MAX_RECORD_BYTES` is a protocol error.

`MAX_RECORD_BYTES` is 1,048,576 bytes. It is an absolute local receive
ceiling, not a value a peer can raise through capability negotiation.
The implementation must also enforce substantially smaller limits for
individual message types, application payloads, file chunks, repeated
fields, strings, identifiers, and padding. Protobuf parser
recursion/total-input limits must be configured where supported. The
receiver must use checked arithmetic and must not allocate based on an
unchecked peer-provided length.

Every announced record must also complete within a bounded receive
deadline. The deadline may be generous and progress-aware for Tor and
other high-latency transports, but a peer must not be able to reserve
memory or a connection indefinitely by announcing a valid large length
and then sending the body arbitrarily slowly.

Each complete record is decrypted and authenticated before any protobuf
field or application data is dispatched. Any failed authentication,
truncation, invalid record size, or stream desynchronization closes the
connection.

The record length is intentionally visible so that the stream can be
parsed. Padding hides the unpadded protobuf size but does not hide the
final padded record size or the timing and direction of traffic.

### Secretstream tag policy

Normal transport records use
`crypto_secretstream_xchacha20poly1305_TAG_MESSAGE`. `TAG_PUSH` is not
used by protocol v3 and receiving it is a protocol error.

`TAG_REKEY` may be used only according to a deterministic implementation
policy shared by both sides or as the tag on a normal record when
explicit rekeying is desired. The receiver accepts it as an
authenticated record and allows libsodium to update the pull state.
Secretstream's automatic internal rekeying remains enabled.

An orderly protocol close sends the final encrypted record with
`TAG_FINAL`. After successfully receiving `TAG_FINAL`, the receiver
accepts no further record on that direction. A transport disconnect
without `TAG_FINAL` is an unclean disconnect, not proof that any
particular application operation failed; application state machines such
as file transfer determine what may be resumed. Authentication failure
or bytes following `TAG_FINAL` are protocol errors.

## Transport protobuf

Every encrypted record contains one `TransportEnvelope` protobuf
message:

``` proto
message TransportEnvelope {
  bytes payload = 1;  // serialized application Envelope
  bytes padding = 2;  // cryptographically random bytes
}
```

The complete transport envelope, including `payload` and `padding`, is
encrypted and authenticated by one secretstream message. Padding is
generated with a cryptographically secure random generator and uses the
same secretstream encryption; no separate padding key is used.

The receiver authenticates the transport envelope, validates its total
size, payload size, and padding size, then deserializes `payload` as an
application `Envelope`. Padding is never passed to the application
layer.

Implementations should use randomized padding to reach size classes,
rather than choosing an unbounded arbitrary padding length. The padding
policy must have a strict maximum and must not allow a peer to force
excessive bandwidth or memory use.

## Application protobuf

Application messages are carried in a typed envelope. The wire schema is
maintained in the protocol protobuf definitions; its shape is equivalent
to:

``` proto
message Envelope {
  uint32 protocol_version = 1;
  uint64 request_id = 2;

  oneof body {
    Capabilities capabilities = 10;
    KeepAlive keep_alive = 11;
    BioRequest bio_request = 12;
    BioResponse bio_response = 13;
    ConnectionRequest connection_request = 14;
    ConnectionResponse connection_response = 15;
    Message message = 16;
    FileOffer file_offer = 17;
    FileChunk file_chunk = 18;
    FileAck file_ack = 19;
    UserInfo user_info = 20;
    Reject reject = 21;
    FileResume file_resume = 22;
    FileResumeResult file_resume_result = 23;
    FileCancel file_cancel = 24;
  }
}
```

The `oneof` provides typed dispatch. A missing body is a protocol error.
Unknown envelope body field numbers are rejected according to the
versioning rules below. If a protobuf implementation accepts multiple
encoded members of a `oneof` and applies normal protobuf last-one-wins
semantics, the application does not require a separate custom wire scan
solely to detect the earlier members. Security validation applies to the
resulting parsed object.

Protobuf parsing success is never sufficient validation. Before
dispatch, handlers must validate field presence, exact identifier/hash
lengths, numeric bounds, enum values, repeated-field counts, string
lengths/encoding policy, authorization, ownership, request correlation,
and state transitions.

The application protocol version is explicit in every envelope. Removed
field numbers and message identifiers are reserved and must not be
reused. Unknown protobuf fields may be discarded, but unknown envelope
message types are rejected.

### Request identifiers

`request_id` is correlation metadata and never an authorization token.

A value of zero means that the envelope is not part of a
request/response exchange unless a specific message definition
explicitly states otherwise. Non-zero request IDs are generated by the
requester and must be unique among that connection's currently
outstanding locally initiated requests.

The implementation must bound the number of outstanding requests. A
response may only consume already-existing locally created pending state
for the matching request ID and expected response type. An unsolicited
response, duplicate response, response with an unknown ID, or response
of the wrong type is a protocol error and must never create pending
state.

Request IDs are scoped to one authenticated connection and do not
survive reconnection. Operations that require reconnection semantics use
explicit stable object or transfer identifiers instead.

The first application message sent in each direction after encryption
must be `Capabilities`:

``` proto
message Capabilities {
  uint32 protocol_version = 1;
  repeated string capabilities = 2;
  uint32 max_record_bytes = 3;
}
```

Both peers must validate the capability declaration before accepting
other application messages. `max_record_bytes` is the sender's
declaration of the largest encrypted record it is willing to receive; it
never raises the local hard receive ceiling. The maximum record the
local side may send is:

``` text
min(LOCAL_MAX_RECORD_BYTES, peer.max_record_bytes)
```

The advertised value must be within a protocol-defined valid range and
large enough to carry mandatory protocol messages. Capability
declarations also have strict limits on total encoded size, number of
entries, individual string length, allowed syntax, and duplicate
handling. Unsupported protocol versions, malformed protobuf, invalid
capability limits, and unsupported required capabilities close the
connection.

## Authorization states

Each connection has an application authorization state independent of
its encryption state:

``` text
HANDSHAKE
    ↓
ENCRYPTED_UNAPPROVED
    ↓
APPROVED
```

After entering `ENCRYPTED_UNAPPROVED`, the following messages are
allowed:

-   `Capabilities`
-   `KeepAlive`
-   public-bio request and response
-   connection request and response
-   explicit rejection or close messages

Ordinary messages, files and file offers, avatars, full `UserInfo`, and
contact-only bio require `APPROVED` state. A message received in the
wrong authorization state is rejected and does not reach the application
handler.

The peer's identity must be authenticated before any pre-approval
message is accepted. Pre-approval messages are subject to the same size,
rate, resource, and state-transition limits as approved messages. In
particular, a peer must not be able to use pre-approval traffic to
create unbounded persistent state, open file transfers, or cause
repeated expensive work.

The implementation must enforce global budgets in addition to
per-identity and per-connection limits. Identities are cheap for an
attacker to create, especially over Tor, so per-peer limits alone are
not a DoS defense. Global budgets include simultaneous handshakes,
simultaneous unapproved connections, pending connection requests,
pre-approval request rate, bytes processed, queued outbound bytes, and
concurrent expensive cryptographic or persistence operations.

## Bio visibility

Bio visibility is an explicit local policy:

-   `CONTACT_ONLY`: the bio is returned only after the peer is approved;
-   `PUBLIC`: the bio may be returned to an authenticated, unapproved
    peer in response to a `BioRequest`.

Bio requests do not automatically reveal a bio during the handshake.
Public bio responses must be bounded and must not implicitly include
private contact identifiers, exact location, private metadata, avatars,
or full user info. Public-bio requests are rate-limited and may be
rejected without changing contact state.

Connection requests and responses must validate the local contact state,
authorization policy, request ownership, and object identifiers before
changing persistent state. Repeated requests are handled according to
the current state; invalid state transitions are rejected.

## Keepalive

`KeepAlive` packets contain a random nonce and random padding. Each peer
sends them independently at a randomized interval between 90 and 240
seconds. They are encrypted and transport-padded like all other messages
and require no response. Tor's circuit maintenance is separate and does
not replace application-stream liveness traffic.

## Files and scheduling

Outgoing work is assigned an internal scheduling class. Protocol v3
requires at least `INTERACTIVE` for control/chat traffic and `BULK` for
file data. `INTERACTIVE` traffic has priority over file offers and file
data. The sender has a bounded socket output queue of 256 KiB. File data
is generated only when the socket output queue has drained sufficiently,
and the scheduler rotates between active file transfers so one transfer
cannot monopolize the connection. The scheduling abstraction must not be
hard-coded around file message types so future realtime and background
traffic classes can be added without redesigning the wire protocol.

At most two file transfers are active per approved contact connection.
Additional transfers remain queued. The application never enqueues an
entire file.

File data is carried in typed `FileChunk` protobuf messages. A chunk
contains an unsigned 64-bit sequence, an unsigned 64-bit byte offset,
the chunk bytes, and a final indicator. The receiver accepts only the
next contiguous sequence and offset. Duplicate chunks are harmless and
are answered with the current cumulative acknowledgement; gaps are
rejected.

The application currently uses 8 KiB file chunks. The file chunk limit
is independent of `MAX_RECORD_BYTES` and must include protobuf and
encryption overhead when enforcing the record limit.

The receiver acknowledges chunks with a typed `FileAck` message
containing the file id, cumulative sequence, cumulative offset, and
final state. The sender may keep eight chunks in flight for
application-level flow control. The sender does not retransmit a chunk
while the encrypted connection remains alive: Tor's underlying reliable,
ordered stream already handles transport loss, and secretstream records
cannot be replayed in place. A delayed acknowledgement does not by
itself indicate that a chunk was lost.

The sender does not mark the transfer complete until the final
cumulative acknowledgement is received and the receiver has validated
the final hash.

Network acknowledgement and crash-recovery durability are separate
concepts. An acknowledgement means that the receiver has accepted and
written the contiguous prefix according to its active transfer policy.
The receiver must checkpoint resumable progress separately, and a resume
offset persisted across application/process failure must never exceed
the prefix actually persisted according to that checkpoint policy. The
protocol must not require an `fsync()` for every small chunk or
acknowledgement, because a hostile peer must not be able to turn the
protocol into an unbounded disk-synchronization workload.

Before offering or sending a file, the sender records a source snapshot
that includes the file's size and modification time. During transfer it
must detect source deletion, read failure, size changes,
modification-time changes, and changes detected by content hashing. If
any of these occur, the sender stops sending chunks and cancels the
transfer with the `SOURCE_CHANGED` file error. The same error is used
whether the source was modified, replaced, deleted, or became
unavailable. A modification-time change is sufficient to cancel even if
the size is unchanged; it is a conservative change detector, while the
SHA-256 hash remains the content-integrity check.

``` proto
enum FileError {
  FILE_ERROR_UNSPECIFIED = 0;
  SOURCE_CHANGED = 1;
}

message FileCancel {
  bytes file_id = 1;
  FileError reason = 2;
}
```

When a connection carrying an active transfer is disconnected, the
receiver may initiate resume after a new authenticated, approved
connection is established. It sends one `FileResume` message for each
transfer it wishes to continue:

``` proto
message FileResume {
  bytes file_id = 1;
  uint64 next_offset = 2;
  bytes sha256 = 3;
  uint64 file_size = 4;
}
```

`next_offset` is the first byte not present in the receiver's durably
stored contiguous prefix for which it has maintained local hash state.
It must be no greater than `file_size` and must satisfy the negotiated
chunking rules. The receiver must not claim an offset beyond its
verified prefix.

The sender compares `file_id`, `file_size`, and the complete SHA-256
hash with the immutable metadata for the offered source file. It must
validate the offset before resuming. If all metadata matches and the
source file is still available, the sender replies with an accepted
`FileResumeResult` and resumes with the chunk beginning at
`next_offset`. If the hash or any other immutable metadata differs, the
sender replies with a cancelled result whose reason is `SOURCE_CHANGED`
and sends no further chunks for that transfer. If the source file has
been deleted or is otherwise unavailable, the sender uses the same
`SOURCE_CHANGED` result. Malformed requests and invalid offsets are
rejected without resuming.

For example:

``` proto
message FileResumeResult {
  enum Status {
    STATUS_UNSPECIFIED = 0;
    ACCEPTED = 1;
    REJECTED = 2;
    CANCELLED = 3;
  }

  bytes file_id = 1;
  Status status = 2;
  uint64 next_offset = 3;
  FileError error = 4;
}
```

When resume metadata differs, the sender sets `status` to `CANCELLED`
and `error` to `SOURCE_CHANGED`. It sends no further chunks for that
transfer. The same `FileCancel` message and `SOURCE_CHANGED` reason are
used when the sender detects a source change during an active transfer.

The receiver may discard its partial file when the sender cancels or
when resume metadata does not match. Chunks received again after resume
are harmless duplicates; the receiver continues to enforce contiguous
sequence and offset validation and sends cumulative acknowledgements.

The file id is the transfer id. Offers carry immutable name, size,
SHA-256 hash, and file-id metadata. Resume is allowed only when the
receiver has a matching transfer metadata record and a partial file with
the expected persisted contiguous prefix; otherwise the partial file is
discarded and the transfer restarts at offset zero. On disconnect,
acknowledged sender offsets are preserved and active transfers return to
the offer/accept state for a verified resume. A transfer that cannot be
resumed returns to the offer/accept state or is cancelled according to
the result status.

### File identifiers, names, storage, and quotas

The file ID is a transfer ID, not a content identity. Transfer IDs are
generated from a cryptographically secure random source, have one exact
protocol-defined length, and must not be reused while retained metadata
for that transfer exists. SHA-256 hashes are exactly 32 bytes. Every
other binary identifier or hash in the protocol must likewise have an
exact specified length and is rejected before use if its length differs.

A peer-provided filename is display metadata, never a filesystem path.
Incoming data is initially written to a locally generated private path
selected solely from local state and a locally validated transfer ID.
Remote filenames are never concatenated with directories and never
control path traversal, extensions used for execution, temporary-file
locations, or overwrite behavior. Platform-specific filename
sanitization happens only when the user chooses to export/save content.

Approved peers remain hostile for resource-control purposes. Local
policy must bound maximum accepted file size, total unfinished incoming
bytes, partial transfer disk usage, queued offers, retained interrupted
transfers, and the lifetime of partial transfers. Peer-declared file
sizes must pass these limits before preallocation or creation of large
persistent state.

The receiver's running SHA-256 state for a partial file verifies what it
has locally processed; it does not independently prove that an arbitrary
prefix matches the sender's advertised whole-file SHA-256. Final content
integrity is established only when the complete file hashes to the
immutable advertised SHA-256.

## Error and close behavior

The connection closes on an invalid or oversized record length, failed
secretstream authentication, truncated record, invalid protobuf,
unsupported protocol version, invalid capability declaration, unknown
envelope body, invalid application field, unauthorized message, invalid
state transition, or invalid file sequence, offset, or transfer
identifier.

No unauthenticated, malformed, or unauthorized payload is dispatched to
the application layer.

## Testing requirements

Protocol tests cover fixed handshake sizes, malformed and replayed
handshake messages, ephemeral-key rejection, transcript binding,
wrong-server identity, signature failure, explicit client key-confirmation failure, implicit server
key-confirmation failure on the first encrypted record, forward-secrecy test fixtures, handshake timeouts and global handshake
limits; record bounds, endianness, record receive deadlines,
secretstream tags/finalization, encrypted transport envelopes, padding
limits, malformed protobuf, parser limits, unknown envelope bodies,
request-ID correlation, unsolicited/duplicate responses, capability
negotiation, pre-approval authorization, public and contact-only bio
visibility, per-connection and global rate/resource limits,
connection-request state transitions, exact identifier/hash lengths,
hostile filenames, disk quotas, file chunk bounds, disconnects, resume
checkpoints, source mutation and deletion during transfer, transfer
fairness, output queue bounds, scheduling-class priority, and
interactive traffic while file transfers are active.

Fuzzing targets include each fixed handshake parser, transport length
framing, secretstream record handling after successful authentication,
protobuf decoding, every application validation boundary,
authorization/state-machine dispatch, request correlation, file-transfer
sequencing/resume, and filename and identifier validation. Fuzz
harnesses must impose their own memory, iteration, and execution-time
limits so malformed input cannot turn a test into an unbounded resource
consumer.

## Future-feature requirements

The following requirements preserve room for the optional/someday
features on the DarkSpeak wishlist without adding their protocol
machinery to v3. They are architectural constraints for future work, not
current feature commitments.

### User identity and multiple devices

The authenticated handshake identity is an endpoint/device identity, not
a permanent assumption that one human user equals one private key. A
future user identity may authorize multiple independent device signing
identities.

Private endpoint keys must not be copied between devices merely to
implement multi-device support. Future device authorization should
support independent addition and revocation of devices and should
cryptographically bind a device identity to the user identity that
authorized it.

Authorization code should therefore distinguish concepts such as
`UserId` and `DeviceId` even if only one of them is exposed by the
current protocol.

### Object authorship and relaying

Connection authentication proves which endpoint is carrying bytes on the
current connection. It must not be treated as permanent proof of
authorship for every application object.

Future groups, channels, distributed libraries, device authorization
records, membership changes, invitations, and administrative actions may
be relayed by peers other than their authors. The application
architecture must allow such objects to carry their own cryptographic
provenance/signatures when required, independent of transport
authentication.

Protocol v3 direct messages do not need redundant application signatures
solely because this future capability may exist.

### Generic content transfer

The hardened file-transfer engine must remain semantically generic: it
transfers a bounded content object from a sender to a receiver. It must
not assume that every transfer originated from a user manually attaching
a file to a chat.

Future chat attachments, shared-directory downloads/uploads, avatars,
thumbnails, and distributed-library objects should reuse the same
bounded, hashed, cancellable, flow-controlled transfer machinery where
practical.

Directory browsing, exchange accounting, library manifests, and similar
higher-level semantics remain separate application protocols layered
above the generic transfer mechanism.

### Transfer identity and content identity

`file_id`/transfer ID identifies one transfer and remains distinct from
content identity. SHA-256 or a future explicitly versioned content
identifier describes content independently of a particular transfer.

This distinction must be preserved so future duplicate detection,
caching, distributed library blocks, multiple sources, manifests, and
resume logic can refer to content without reusing ephemeral transfer
state.

### Distributed libraries and third-party storage

Future distributed-library content may be downloaded from peers that did
not author it. Content authenticity must therefore be capable of
depending on signed manifests and content hashes rather than on trust in
the peer that happened to transport a block.

A storage peer must never gain authority over the meaning, ownership, or
authorship of an object merely because it supplied the object's bytes.

### Avatars and thumbnails

Avatars and thumbnails are hostile binary content. Prefer representing
them as bounded content objects referenced by metadata/content
identifiers and transferring them through the generic content-transfer
machinery rather than placing arbitrarily large image bytes directly in
ordinary control messages.

Rendering policy must independently restrict accepted formats, encoded
size, decoded dimensions, memory use, and parser behavior. Rich active
formats must not gain execution or network-fetch capability merely
because they are used as profile or thumbnail content.

### Shared directories and exchange accounting

Shared-directory browsing and upload/download permissions are
authorization features above the byte-transfer layer. Remote names
remain display metadata and never become trusted local paths.

Future quota or exchange-credit systems must consume locally verified
transfer facts, such as authenticated peer/device, validated byte count,
and completed content hash. Peer-declared sizes or claimed completion
must never directly create credit or bypass quotas.

### Groups and identifier namespaces

Future groups, channels, libraries, users, devices, transfers, and
content objects introduce distinct identifier namespaces.
Implementations should use strongly distinct internal types such as
`UserId`, `DeviceId`, `GroupId`, `LibraryId`, `TransferId`, and
`ContentId` rather than one generic byte/string identifier type.

An identifier validated for one namespace must never be accepted
implicitly in another.

### Realtime audio/video and scheduling

Protocol v3 does not add generic multiplexing or realtime media framing.
However, the secure-record layer must remain conceptually independent of
the protobuf application layer so a future authenticated media/multiplex
layer can be introduced without coupling cryptographic session
establishment to `Envelope`.

Outgoing scheduling must use abstract traffic classes rather than
hard-coded file checks. Protocol v3 needs only interactive and bulk
behavior, but the implementation should permit future realtime, normal,
bulk, and background classes.

A future realtime transport may have requirements different from a
reliable ordered byte stream. Do not generalize v3 prematurely to
unreliable packet transport; instead keep the current privacy transport
behind a clean reliable byte-stream interface and introduce a separate
transport capability only if a future feature actually requires it.

### Privacy transports

Application framing, endpoint authentication, authorization, request
state, content transfer, and object provenance must remain independent
of Tor. Tor-specific stream isolation, onion addressing, SOCKS
configuration, connection establishment, and circuit lifecycle belong to
the Tor transport backend.

Future privacy-strengthening transports should be replaceable beneath
the secure DarkSpeak session when they can provide the required reliable
ordered bidirectional byte-stream semantics. Transport-specific liveness
and keepalive policy may differ without changing application message
semantics.
