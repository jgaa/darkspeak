# DarkSpeak

Secure Instant Messenger based on the legacy Tor Chat protocol.

# Mission Statement

To make the most Secure and Private Communication Platform for citizens, in the History of the World.

# Planned features
- Secure Instant Messaging (Legacy Tor Chat)
- File Transfer (Legacy Tor Chat)
- More than one ID on one Tor instance (require use of other ports - only one ID will be visible from legacy Tor Chart clients)
- Proxy service (allow darkspeak on several devices simultaneously with the same ID)
- Nice looking graphical interface on Linux, OS/X and Windows
- Android version
- IOS version
- Groups (like in Skype - several participants in one chat)

# Enhancements over the original Tor Chat

Dark Speak will remain backwards compatible with the original Tor Chat
protocol. However, it will implement some extra guards to protect the
user, and new protocol features that can be used when both parties use
Dark Speak (or another enhanced client).

- Improved security in the client to prevent DoS attacks, memory overflow etc.
- Better authentication of peers (to make it harder or impossible to impersonate a person).
- Another layer of PKS p2p strong encryption and secure authentication.
- Emergency flag to indicate that the conversation is compromized (for example by
    the physical presenc of an adversary).

# Some ideas for future versions
- Support for more Tor IM protocols (like ricochet)
- Support for other anonymous networks
- Server versions where oprganizations (like newspapers) can host thousands of ID's on one Tor instance (like darkspeak:jgaa@23enroxotjtsogn4)

# Current State
The project is under heavy development. I'm building a GUI based on QT Quick.
I hope that I can use basically the same GUI source code on all target platforms.
So far it looks promising. (The learning curve for QT Quicks is steeper the I expected,
but the GUI is slowly becoming usable)

# Building from source
- [kubuntu 16.10](doc/darkspeak-kubuntu.md)
- [Microsoft Windows](doc/darkspeak-windows.md)


# Integrating with a Tor Service

Get the Tor service. Most Linux distributions will offer it as a package.

Follow the instructions [here](https://www.torproject.org/docs/tor-hidden-service.html.en) to create a service.

A Tor config file (torrc) may look like:

```
# torrc example for Debian Gnu Linux
HiddenServiceDir /var/lib/tor/darkspeak
HiddenServicePort 11009 127.0.0.1:11009
```

After altering torrc and re-starting tor, you need to look at the
hostname file in your hidden service dir. The hostname, without the
".onion" postfix is your chat handle (TC id).

Start darkspeak, open the settings/You dialog and paste your chat handle
into the "Chat Id" field. Then save and press the online button.

Example under Linux:
```sh
$ sudo sh -c 'echo "HiddenServiceDir /var/lib/tor/darkspeak
HiddenServicePort 11009 127.0.0.1:11009" >> /etc/tor/torrc'
$ sudo systemctl restart tor
$ sudo cat /var/lib/tor/darkspeak/hostname
```

# Roadmap
## Beta 1a
- [x] ~~Document the legacy Tor Chat protocol~~
- [ ] Implement a library that speaks the TC protocol
 - [x] ~~Core functionality (work in progress)~~
 - [ ] Unit tests
 - [ ] Functional tests
- [x] ~~Implement GUI for Linux~~
- [x] ~~Implement GUI for Windows~~
- [ ] Implement GUI for OS/X

## Beta 1b
- [ ] Implement GUI for Android
- [ ] Implement GUI for IOS

## Beta 2
- [ ] Add good/popular suggestions/ideas
- [ ] Add DarkSpeak specific extentions to the TC protocol
 - [ ] Group chat
 - [ ] End to end PKS encryption
 - [ ] Fingerprint verification of peers
 - [x] Research how to configure an existing Tor Server seamlessly with the hidden service
 - [ ] Feed own cert to Tor on Demand
 - [ ] Implement support for multiple ID's in one client
- [ ] Package the application. Make sure it's easy to get started.

## Beta 4
- [ ] Add good/popular suggestions/ideas
- [ ] Implement proxy (master node) functionality, so that we can have several clients running with one ID.

## Release version 1
- [ ] Fix all known bugs.


# Links
 - The reference implementation of [TorChat](https://github.com/prof7bit/TorChat).
 - Partially description of the [TC protocol](https://www.meebey.net/research/torchat_protocol/).
