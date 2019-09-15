# Dark Speak protocol

This document describes the protocol for the DarkSpeak secure instant messenger.

Status: Draft in progress

DarkSpeak use the Tor network as it's transmission layer. That means that it gets anonymity and encryption for 'free'. Each client instance can create one or more Tor Hidden services and listen for incoming connections on those services. On startup, it can also connect to peers in the contact-list's hidden tor service addresses, and try to establish connections.

One connection is sufficient between two peers. If two peers collide (both connects to each others at the same time), the peers will both do a string-compare of the unique session-cookies as soon as the collision is detected, and the connection with the smaller value will be dropped.

In addition to using the encryption of the Tor hidden service, DarkSpeak adds an additional layer of security by encrypting the payload of all messages sent to a recipient with the recipients public key. That means that not even the local Tor server can intercept the actual traffic between peers.

Clients using DarkSpeak are identified by their public key. A contact handle contains that key in full, and one or more Tor hidden services where that user may be contacted. The hidden service use a random port to make it harder to scan the Tor network for DarkSpeak instances.

## Layers
The protocol have 3 layers

- Transport
- Packet
- Conversations

## Transport Layer

The Transport Layer is a simple binary format with a header and a payload.

The header is a flat, binary structure with the following fields:

- **version** 1 byte
- **size**    2 bytes unsigned integer in Big Endian format. Size in bytes of the payload.
- **channel** 2 bytes unsigned integer in Big Endian format
- **md5**    16 bytes md5 checksum for the remainder of the packet.

The payload follows directly after the header.

- **payload** The payload.

**Version**: Version of the Dark Speak protocol used.

**Size**: Size of the payload in bytes.

**Md5**: Checksum to detect transmission errors. Since the checksum is encrypted it is tamper-proof, and should be safe to use. We use prefer md5 here because it is fast to calculate and occupies only 16 bytes.

**Channel**: This is used to route the message to an appropriate handler.
- Channel 0 is the control channel. This channel require the payload to be in json format.
- File transfer handles are used to transfer raw chunks of data from a file

Channels are allocated incrementally, and may wrap around. Only one channel with a  given id can be active at any time.

**Payload**: Blob of data. It consists of exactly the number of bytes specified in the size field, minus the encrypted part of the header (currently 19 bytes).

## Packet layer
The packet layer is used to send requests and responses. The packages are encoded as json, and initially the encoding must be us-ascii.

The following packets are defined for the Control Channel #0

**Hello**: This package is the first package sent from the client to the server. The server will not respond to a client unless it sends a valid Hello message, encrypted with the servers public key. This makes it harder to scan the Tor network for DarkSpeak instances. If an invalid message is received, the server will drop the connection. The server will also drop the connection shortly after it is established if no Helo message is received.

```
{
    "type" : "Helo",
    "pubkey" : "GHGHGHG.....",
    "name" : "jgaa",
    "cookie" : "sjdf898"
}

```

- Type: Helo
- pubkey: Public key for the client
- name: The current nickname for the client
- cookie: Random session cookie

The pubkey is used to identify the client. If the client is known and on the contact-list, the server will send a *Greetings* message. If the client is unknown, it will send a *Unauthorized* message. If the server detects a collision, where it is currently trying to connect to the same client, it will compare the session-cookie, and reject the connection if it has the smaller value.

Response:

```
{
    "type" : "Ack",
    "what" : "Helo",
    "status" : "Greetings" | "Unauthorized" | "Collision"
}
```

If the server does not want to talk to the client (for example if the client is black-listed), the connection is dropped without any explanation.

**AddMe**": This message asks the server to add the client to the contact list.

```
{
    "type" : "AddMe",
    "name" : "jgaa",
    "address" : [ "ds://33oa7kr63jpwpwp5:5432" ],
    "message" : "Please add me",
    "message-encoding" : "us-ascii" | "utf-8"
}
```

- Type: AddMe
- name: The nickname currently used by the client
- address: List of addresses where the contact can be reached.
- message: A message to show in the connect request. This can for example be a code, explanation or secret that identifies the person.
- message-encoding: Encoding for the message

Response:

```
{
    "type" : "Ack",
    "what" : "AddMe",
    "status": "Added" | "Pending" | "Rejected" | "Blocked"
}
```

If the status is "pending", the client can expect a new message from the server when the user has decided if he wants to add the person to his contact list.

If a AddMe is sent to a server that already have added the contact, an acknowledgement is sent in return.

**SetAvatar**: This message sends an avatar image to be used for the contact.

```
{
    "type" : "SetAvatar",
    "height" : 64,
    "width" : 64,
    "r": base-64 encoded data,
    "g": base-64 encoded data,
    "b": base-64 encoded data,
    "a": base-64 encoded data
}
```
The data is a base-64 encoded image sent as 4 x 8-bit channels, without any other encoding. This is simply to avoid exploits in the image decoding libraries. The sender specifies the width and height of the image, and the receiver may or may not use the image.

The image may be from 64 to 128 pixels hight to 64 - 128 pixels width.

Reply:

```
{
    "type" : "Ack",
    "what" : "SetAvatar",
    "status" : "Accepted" : "Rejected"
}
```

**Invite**: Invitation to join a conversation.

```
{
    "type" : "Invite",
    "conversation" : "67676ggg"
    "subject" : "Meeting with people",
}
```

- type: Invite
- conversation: Unique id of an active conversation at the host
- subject: Optional subject for the conversation.

A conversation is a group chat. The group chat is hosted by one participant, and all messages are sent to that node. Only contacts on that persons contact-list can participate in the conversation. All the messages in the conversation is sent to all the participants by the hosts instance of DarkSpeak.

Reply:
```
{
    "type" : "Ack",
    "what" : "Invite",
    "conversation" : "67676ggg"
    "status" : "Pending" | "Rejected"
}
```

The status in the reply indicates if the person has responded to the request, or rejected the invite. If the person want to join, there will be one Ack with status Pending, and then a separate Join message.

The status joined means that the conversation is set up by the participant, and that messages from that conversation can be handled.

**Join**: Request to join an existing conversation

```
{
    "type" : "Join",
    "conversation" : "67676ggg"
}
```

- type: Join
- conversation: The handle of an active conversation at the host.

Response:

```
{
    "type" : "Ack",
    "what" : "Join",
    "conversation" : "67676ggg",
    "status" : "Joined" | "Rejected"
}
```

The reason for the rejection is not provided. It could be that the user was not invited, or that the conversation does not exist, or that it is closed.

**Message**: A message

```
{
    "type" : "Message",
    "message-id" : "sjdaghfjasghdf87",
    "date" : 2347263476,
    "content" : "bla bla",
    "encoding": "us-ascii" | "utf-8",
    "conversation" : "67676ggg",
    "from" : "...",
    "signature" : "..."
}
```

- what: Message
- message-id: Unique message id for the host. The host must never re-use the message-id for another message. Used to keep track of messages and to avoid duplicates if connections are unreliable.
- date: The date / time the message was composed. Unix time_t value, truncated to whole minutes to make it harder to perform statistical analyzes on the DarkSpeak transactions.
- content: Content of the message
- encoding: The encoding of the content. Some clients may reject anything but us-ascii to prevent exploits in the Unicode stack of system or QT libraries.
- conversation: Optional parameter for messages that are part of a group chat.
- signature: The message, except the signature entry, is signed by the sender to verify the authenticity of the message. This is required if the message is relayed, for example by a chat host.
- from: Hash of the senders public key. Used to identify the sender in a group chat.

Reply:

```
{
    "type" : "Ack",
    "what" : "Message",
    "status" : "Received" | "Rejected" | "Rejected-Encoding",
    "data" : "..."
}
```

- data: message-id of the relevant message


**IncomingFile**: Request to send a file to the recipient.

```
{
    "type" : "IncomingFile",
    "name" : "cutecat.jpg",
    "sha256" : "GfE6564...",
    "size" : 12345,
    "file-type" : "binary" | "text",
    "rest" : 50,
    "file-id" : "123"
}
```

- what: IncomingFile
- name: Suggested name of the file. Cannot contain slashes. May be restricted to us-ascii at some clients.
- sha256: Sha256 hash of the file, encoded as base64
- size: Size of the file in bytes
- type: Binary or text. DarkSpeak will convert text-files to UNIX format, and save in the local format for the operating system used by the client. This feature makes it harder to deduce the operating system used by a client.
- rest: Restore point. Used to continue an aborted or incomplete transfer. The transfer will start at the file-offset (in bytes) specified here.
- file-id: Used by the client to track the status of a file-transfer.

Reply:

```
{
    "type" : "Ack",
    "what" : "IncomingFile",
    "data" : "123",
    "status" : "Proceed" | "Rejected" | "Rejected-Encoding" | "Completed" | "Failed | "Abort" | "Resume"
    "channel" : 5,
    "rest" : 123
}
```

The payload of a file-transfer is sent directly over the transport-layer, using the channel in the Ack.

Normally there will be two Ack messages for a transfer, first one with status *proceed*, and and then one with status *completed*.

If the connection between two parties are broken and reestablished, the receiving part may send another Ack message, requesting *resume* of the specified file offset in the optional field *rest*. The sender will then start sending from the specified file-offset.

- data: File-id

**SendFile**: Request to send a file.

DarkSpeak can act as a simple file-server, and this request asks the server to send a file to the client.

```
{
    "type" : "SendFile",
    "name" : "/pub/cats/cutecat.jpg",
    "file-id" : "123",
    "channel" : 16,
    "rest": 100
}

```

- type: SendFile
- name: Full path to the requested file. The path is always in UNIX format.
- file-id: Used by the client to track responses regarding the transfer
- channel: Where to send the payload of the file

Reply:

```
{
    "type" : "Ack"
    "what" : "SendFile",
    "file-id" : "123",
    "channel" : 16,
    "status" : "sending" | "rejected"
}
```

**ListDir**: Request to provide a directory-listing for available files.

```
{
    "type" : "ListDir",
    "path" : "/pub/cats",
}
```

Reply:

```
{
    "type" : "Ack",
    "what" : "ListDir",
    "path" : "/pub/cats",

    "list" : [
        {"name" : "cutecat.jpg", "type" : "file", "size" : 12345, "Sha512" : "a769a1c29ba5428c7f4e260db495c040641404d9..."},
        {"name" : "even.cuter.cats", "type" : "dir"},
    ]
}

```

## The Conversation Layer

Conversations can persist over time. Most conversations are unnamed, and between two participants. DarkSpeak also support group chat. Such conversations are hosted by a host. All messages are sent to the host, and then resent to all the participants.

Messages are always queued in the client, and tracked by the client until they are received by all participants or timed out. The client will make it's best effort to deliver each message to each device for each participant.

TBD: Specify how several devices for one person synchronizes the contact-list and messages. When one device adds a contact or a message, it should broadcast that event to the other devices as well. When a message is read, it should mark it as read on all the devices.

## Isolation

If a user have several identities in one client, they are totally isolated. No contacts or shared directories are shared among such identities. The idea is that a person should be able to be online with several identities without having anything that connects them to the same person.

