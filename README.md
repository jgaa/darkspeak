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
## Debian
TBD

## Windows.
I am using 64 bit Windows 10 with Visual studio 14 (2015).
- Get Microsoft Visual Studio 14 (2015) or newer (I'm using the free community version)
- Get and compile boost (www.bost.org) for 64 bits
- Get cmake
- Get QT Studio for 64 bit Windows (I am using QT 5.6)
- Get the source code from github, initialize the dependencies):
```bash
$ git clone https://github.com/jgaa/darkspeak.git
$ cd darkspeak
$ git submodule init
$ git submodule update
```
- Start Cmake (I use the Cmake GUI under Windows)
In Cmake, add the following paths (I am showing the paths on my development machine):
```
BOOST_ROOT path C:/devel/boost_1_60_0
BOOST_LIBRARYDIR path C:/devel/boost_1_60_0/stage/lib/x64
CMAKE_PREFIX_PATH path C:/Qt/Qt5.6.0/5.6/msvc2015_64
```
- Configure and generate in Cmake
- Find darkspeak.sln (it will be in darkspeak/build on my system) and double click on it to start Vu=isual Studio
- In Visual Studio, build the solution.

# Roadmap
## Beta 1
- [x] ~~Document the legacy Tor Chat protocol~~
- [ ] Implement a library that speaks the TC protocol
 - [x] ~~Core functionality (work in progress)~~
 - [ ] Unit tests
 - [ ] Functional tests
- [x] ~~Implement GUI for Linux~~
- [x] ~~Implement GUI for Windows~~
- [ ] Implement GUI for OS/X
- [ ] Implement GUI for Android
- [ ] Implement GUI for IOS

## Beta 2
- [ ] Add good/popular suggestions/ideas
- [ ] Add DarkSpeak specific extentions to the TC protocol
- [ ] Implement support for multiple ID's in one client
- [ ] Research how to configure an existing Tor Server seamlessly with the hidden service
- [ ] Package the application. Make sure it's easy to get started.

## Beta 3
- [ ] Add good/popular suggestions/ideas
- [ ] Implement encryption from client to client with PKS. Remember peer's public keys (like ssh).
- [ ] Implement group chat

## Beta 4
- [ ] Add good/popular suggestions/ideas
- [ ] Implement proxy (master node) functionality, so that we can have several clients running with one ID.

## Release version 1
- [ ] Fix all known bugs.


# Links
 - The reference implementation of [TorChat](https://github.com/prof7bit/TorChat).
 - Partially description of the [TC protocol](https://www.meebey.net/research/torchat_protocol/).
