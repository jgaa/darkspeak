# DarkSpeak

Secure Instant Messenger based on the legacy Tor Chat protocol

# Planned features
- Secure Instant Messaging
- File Transfer
- Proxy service (allow darkspeak on several devices simultaneously with the same ID)
- Nice looking graphical interface on Linux, OS/X and Windows
- Android version
- Groups (like in Skype - several participants in one chat)

# Encahcements over the original Tor Chat

Dark Speak will remain backwards compatible with the original Tor Chat
protocol. However, it will implement some extra guards to protect the
user, and new protocol features that can be used when both parties use
Dark Speak (or another enhanced client).

- Improved security in the client to prevent DoS attacks, memory overflow etc.
- Better authentication of peers (to make it harder or impossible to impersonate a person).

# Roadmap
## Beta 1
- [ ] Document the legacy Tor Chat protocol
- [ ] Implement a library that speaks the TC protocol
 - [ ] Core functionality
 - [ ] Unit tests
 - [ ] Functional tests
- [ ] Implement GUI for Linux
- [ ] Implement GUI for Windows
- [ ] Implement GUI for OS/X
- [ ] Implement GUI for Android

## Beta 2
- [ ] Add good/popular suggestions/ideas
- [ ] Implement support for multiple ID's in one client
- [ ] Research how to configure an existing Tor Server seamlessly with the hidden service
- [ ] Package the application. Make sure it's easy to get started.

## Beta 3
- [ ] Add good/popular suggestions/ideas
- [ ] Implement proxy (master node) functionality, so that we can have several clients running with one ID.

## Release version 1
- [ ] Fix all known bugs.


# Links
 - The reference implementation of [TorChat](https://github.com/prof7bit/TorChat).
 - Partially description of the [TC protocol](https://www.meebey.net/research/torchat_protocol/).
 