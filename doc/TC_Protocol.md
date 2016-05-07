# Tor Chat Protocol

This document is based on other peoples research in this area, and on
my own examination of the original Tor Chat Source Code.

The description here is not entirely correct. TBD: Correct it!

# Credits
- Original Source Code [TorChat](https://github.com/prof7bit/TorChat).
- Mirco Bauer [TC Protocol](https://www.meebey.net/research/torchat_protocol/)

# General

- Uses TCP port 11009 over Tor
- Text is sent UTF-8 encoded
- The protocol is line based: command [sp] payload [lf]
- lf = 0x0a
- ''command'' may only consist of characters [a..z] or _
- If an incoming message with an unknown command is receido nothing and just reply with "not_implemented"

- Criteria for valid TC ID:
```Python
def isValidAddress(self):
        if len(self.address) <> 16:
            return False
        for c in self.address:
            if not c in "234567abcdefghijklmnopqrstuvwxyz":  # base32
                return False
        return True
```

Incoming messages have the blob encoded in LF format. This is decoded to binary data
when the message is received.

TC originally instantiates a class for each incoming message, and then "executes" it.
The default implementation is to reply "not implemented".

# Commands
## add_me

This must be sent after connection if you are (or want to be)
on the other's buddy list. Since a client can also connect for
the purpose of joining a chat room without automatically appearing
on the buddy list this message is needed.


```
Command: add_me
```

TC will automaticall add the buddy to the buddy-list and reply with
a chat message: "<- has added you"

## client

Transmits the name of the client software. Usually sent after the pong

```
Command: client "software_name"
```

## filedata

After a file-name message has initiated the transfer several
filedata messages transport the actual data in blocks of fixed
size. start is the byte offset of the block in the file and hash
is an md5 hash of the block used as a check-sum. Each message must
be answered with file-data_ok after successfully verifying the hash.
The sender should send only a limited number of blocks ahead of
incoming ok messages (for example send the 5th block only after
the 1st is confirmed, the 6th only after the 2nd confirmed, etc.,
this number is only a wild guess and might need some tuning)

Start is "block start position in bytes". (File offset?)

```
Command: filedata <transfer_cookie> <blob (fixed size)> <hash> <start>
```

## filedata_error

This is sent instead of filedata_ok when the hash was wrong or the block start
was later than what we would have expected (entire blocks have been skipped/lost
due to temporary disconnect). A file sender must react to this message by
restarting the file transmission at the offset given in start. A file receiver will
send this message whenever it wants the the transfer restart at a certain position.

Start is file offset to resume the transfer.

```
Command: filedata_error <transfer_cookie> <start>
```

## filedata_ok

Every received "filedata" must be confirmed with a "filedata_ok"
(or a "filedata_error") message. A File sender will use these messages
to update the sending progress bar and to know that it can send more
blocks.

Start is "block start position in bytes". (File offset?)

```
Command: filedata_ok <transfer_cookie> <start>
```

## filename

The first message in a file transfer, initiating the transfer.
Note that File transfer messages are the only messages that are allowed
to be sent out on the incoming connection to avoid delaying of chat messages.

Each transfer has a unique cookie, made up by the sender.

The file-name is UTF-8 encoded.

```
Command: filename <transfer_cookie> <file_size> <block_size> "filename"
```

## file_stop_sending

A file receiver sends this to make the file sender stop sending,
a file sender must react to this message by stopping the file sending,
the GUI should notify the user that the receiver has canceled. This
message usually occurs when a file receiver clicks the cancel button.

```
Command: file_stop_sending <cookie>
```

## message

this is a normal text message. Text is encoded UTF-8

```
Command: message "text"
```

TC will reply with a Chat message explaining that the message
was not delivered if the sender is not in the buddy list.

## ping

A ping message consists of sender address and a random string (cookie).
It must be answered with a pong message containing the same cookie to so that
the other side can undoubtedly identify the connection

```
Command: ping <tc-id> <cookie>
```

TC implements a check to see if incoming connections try to fake ping
messages - that is, try different tc_id values on the same connection.
Since the TC protocol is peer-to peer, a legitimate client will only
ping using one <tc-id>, namely it's private TOR secret service id.

TC also search all active connections to see if there is already a connection
from the <tc-id>. If there is, the message is regarded as fake. In this case,
it also sends a "not_implemented double connection" message to the existing
peer.

When TC receives a ping message, it put the connection in a temporary
conntion-list and reply with pong, unless we have already sent a pong
and there has not been a problem with the connection to the peers hidden
service.

## pong

```
Command: pong cookie
```

Incoming pong messages are used to identify and authenticate
incoming connections. Basically we send out pings and see which
corresponding pongs come in on which connections.
we search all our known buddies for the corresponding random
cookie to identify which buddy is replying here.

TC does some checks to see that the cookie comes from the correct
buddy.

## profile_avatar

the uncompessed 64*64*24bit image. Avatar messages can
be completely omitted but IF they are sent then the correct
order is first the alpha and then this one

```
Command: profile_avatar blob
```

## profile_avatar_alpha

This message has to be sent BEFORE profile_avatar because
only the latter one will trigger the GUI notification.
It contains the uncompressed 64*64*8bit alpha channel.
this message must be sent with empty data (0 bytes) if there
is no alpha, it may not be omitted if you have an avatar.
It CAN be omitted only if you also omit profile_avatar

Command profile_avatar_alpha blob

## profile_name

Optional command

```
Command: profile_name "Name"
```

## profile_text

Optional command

```
Command: profile_name "Text"
```

## remove_me

when receiving this message the buddy MUST be removed from
the buddy list (or somehow marked as removed) so that it will not
automatically add itself again and cause annoyance. When removing
a buddy first send this message before disconnecting or the other
client will never know about it and add itself again next time

```
Command: remove_me
```

## status

transmit the status, this MUST be sent every 120 seconds
or the client may trigger a timeout and close the connection.
When receiving this message the client will update the status
icon of the buddy, it will be transmitted after the pong upon
connection, immediately on every status change or at least
once every 120 seconds. Allowed values for the data are
"avalable", "away", "xa", other values are not defined yet.

```
Command: status "available" | "away" | "xa"
```

## version

Transmits the version number of the client software. Usually sent after
the 'client' message

```
Command: version "version"
```

# Replies
## not_implemented

Anything the receiver don't understand

# Authentication

## Alice connects to Bob.

- Alice connects to Bob's hidden Tor Service - hence, she trusts Bob to be Bob.
- Bob sends a ping message with a hard-to-guess cookie to Alices hidden Tor
    Service.
- Alice Sends a pong message to Bob, repeating the cookie. Since only Alice can
    know the cookie, Bob now trust Alice to be Alice.

# Security considerations

There are no size-limits on messages, rate-limits on how many messages a peer
(or the world) can send, and no way to detect if Alice or Bob has been
compromized. There is also no connection limit. Such limits must be
offered in the UI and enforced in the client.

Contacts (buddies) seems to be added automatically, something that opens up
for DoS attacs by sending a huge number of add_me messages from a bot-net.

There are no filters to allow only known buddies, or white-listed buddies to connect.
We need to add both.

UTF-8 unicode filenames may pose a threat to the receiver, as the files
may masquerade as having different locations/names than they actually do. They
may also try to exploit vulnerabilities on the UTF-8 decoder. We need some kind of
sanity check to make sure that files are only written to the intended destination
folder, and a check to make sure that the encoding is valid.

UTF-8 encoded messages are vulnerable to UTF-8 decoder vulnerabilities. We need
to make sure that the encoding is valid, and look for known exploit patterns in
the messages.


# Work-flows
## A client starts up and connects to the Tor network

## Someone knows a Tor Chat ID and want to initiate a conversation

Speculative...

```
Alice -- [ ping ] --> Bob
Bob -- [pong ] --> Alice
Alice -- [ client ] --> Bob
Alice -- [ version ] --> Bob
Alice -- [ add_me ] --> Bob
Bob --> [ Msg: Was added ] --> Alice
```

TBD: See what goes on in a real conversation

## A client initiates a conversation with an existing contact

Speculative...

```
Alice -- [ ping ] --> Bob
Bob -- [pong ] --> Alice
Alice -- [ client ] --> Bob
Alice -- [ version ] --> Bob
Bob --> [ Msg: Was added ] --> Alice
```

TBD: See what goes on in a real conversation


## A client transfers a file to an existing contact

TBD
