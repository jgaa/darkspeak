 

# Features

Things not in the backlog are scheduled for Beta.

## Operating systems / Platforms
- [x] Ubuntu / Debian
- [ ] Suse
- [ ] Centos
- [ ] Linux appimage
- [x] Macos
- [ ] Windows

Beta II
- [ ] Android
- [ ] IOS

## Network

**Tor**
- [x] Use the system's Tor server
- [ ] Use external Tor server via username / password
- [x] Support legacy Tor addresses / keys
- [ ] Make an option for what address-type we want in app-settings, and enforce that when we create Tor services.
- [ ] Allow use of external tor binary under our control (Configure the server, start it and stop it)

Backlog
- [ ] See if we can embed Tor into the application as a library.
- [ ] Support new Tor addresses/keys

## Application

**On startup**

Identities

- [x] Setting to determine if we go online automatically

Backlog
- [ ] Setting to determine if we take identities online immediately, or by random delays, and if so, what the delay-ranges are

## Identities

**Create identity**

Creates a new identify for a user of the application. 

- [x] Creates a new private and public key.
- [x] Creates a new hidden Tor service.
- [x] Creates an entry in the local identities table in the Sqlite storage.
- [x] Deny creation of existing name

**Delete an identity**
- [x] Context menu
- [x] Confirm this via input dialog
- [ ] Delete all contacts associated with this identity
- [ ] Delete all conversations for this identity
- [ ] If there are files, ask if they should be deleted as well, and delete them if conformed. 

**Export an identity**
- [ ] Context menu
- [ ] Let the user select a file name and destination directory for the export
- [ ] Export the keys and let the user choose if he wants to export the Tor hidden service and the associated contacts. 

**Import an identity**
- [ ] Context menu
- [ ] Import from export-file (simple json format, matching the Identity data object and an array of contacts). We import only the identity data and contacts information, no conversations).
- [ ] If no hidden service was exported, create a new hidden service. 

**Create a new Tor hidden service**
- [x] Context menu
- [x] Ask the user to confirm.
- [x] Create a new Tor hidden service and replace it with the old one. If the old one was on-line, shut it down before deleting the key. 

**Import a Tor hidden service**
- [x] Context menu
- [x] Import the Tor hidden service via a dialog where the hidden service identifier (onion address), port and private key is provided. Initialize the port field with a valid, random port.

Backlog
- [ ] Allow the user to import a file created from the output of eschalot directly. Select a random port.

**Copy identity as Base58 data.**
- [x] Context menu
- [x] Create a binary array with the public key and the onion address and the port, encode it to base58check, and copy it to the clipboard. Prefix the data with the users nickname. 
- [ ] Same as above, but save as a file. Let the user select file-name and directory.

**Display identity in the UI**
- [x] Show the nickname.
- [x] Show the base58check encoded pubkey
- [x] Show the Onion address and port
- [x] Show online-status for the Tor hidden service
- [x] Show a slider / checkbox that enables / disable auto-connect for the identity
- [x] Show the avatar image if the user has added one

**Select avatar image**
- [x] Provide a file-dialog where the user can select an avatar image. 
- [x] Scale and crop the image to 128 x 128 pixels and store it in the database.
- [x] Update the UI with the new avatar image.
- [x] Notify connected contacts about the change.

**Change nickname**
- [x] Edit Identity dialog
- [x] Provide a input where the user can enter a new nickname.
- [x] Update the database and UI.
- [x] Notify connected contacts about the change.
- [x] Notify contacts about the new nickname when connecting

**Connect / Disconnect Tor hidden service**
- [x] Context menu. (Show text "Connect" | "Disconnect" depending on state)
- [x] Start connecting or disconnecting depending on current state.
- [x] Start / stop the Tor hidden service for that identity
- [x] Start a local socket for each identity and use that for the Tor service


## Contacts

**Add a contact**

- [x] Add "Add Contact" icon in the toolbar
- [x] Check the clipboard for a valid contact (name:base58pubkey) and offer to use it if it exists.
- [x] Show a contact dialog with nickname (read only), name, transport, base58pubkkey, notes, Attributes: blocked, Favorite, autoconnect
- [x] Provide a means to add a message with the addme request
- [ ] For default name, show sequence numbered nickanme if the name is in use
- [ ] Validate input: Name (existing name), hash from pubkey (existing contact), onion address correctness, handle correctnes. Ony allow to proceed when all validations are OK.

**Present contacts**
- [x] Show a view similar to the Identities list (verbose)

Backlog
- [ ] Show a toggle verbose/brief view mode icon in the toolbar
- [ ] Show a shortlist with a smaller avatar, name and blocked / unread messages icon

**Copy contact**
- [x] Add context menu
- [x] Copy nickname:base58pubkey to the clipboard

**Connect / disconnect**
- [x] Add context menu
- [x] Add functionality to establish a connection with the contact

**Autoconnect**
- [x] If the autoconnect flag is set, connect to this client when we go online
- [ ] Option in app-settings to determine if we connect immediately, or by random delays, and if so, what the delay-ranges are

**Block contact**
- [ ] Provide a button in the contact dialog to block the contact
- [ ] If blocked, disallow the client to connect to us.
- [ ] If blocked, disable the connect context menu and the autoconnect feature
- [ ] Show a bloched icon where we usually show the online status in the contact listings

**Online status**
- [ ] Show an overlay icon over the avatar with the online status: contacting, disconnected, offline, connecting, online, rejected
- [x] Update the status as the Transport layer notifies us about changes

**Last seen**
- [ ] Update a property for the contact as the Transport layer notifies us about changes

**Unread messages**
- [ ] Update a property for the contact if he has unread messages. Do not flag this if the associated messages view is active and have focus

**Remember selected contact for identity**
- [ ] When changing identity, remember the selected contact (by hash), and try to re-select it when we change back to the same identity. 

**Search / filter**
- [ ] Add a serach-bar to find contacts fast, and show only matching contacts.

**Accept an AddMe request**
- [x] Accept an addmerequest in the UI and add the contact to the appropriate identity. 

**Name**
- [x] The user can chose a name for the contact. That name is used if set.
- [x] If a contact changes his nickname (name of his identity), the new name is shown in the contact-list under 'Nick'. 


## Notifications

Notifications are shown in the home tab, grouped on identity.
- [x] Notification for incoming contact request
- [ ] Notification for incoming file
- [ ] Notification for unread message(s). Only one notification per contact.
- [ ] When new messages arrive, show the notification for the latest message
- [ ] If enabled in app-settings, show notifications via system-notifications as well.
- [ ] If a contact changes his name, and we have defined a name for that contact, add a notifcation asking if we should change the name, or just automaticllaly use the contacts choosen nick.
- [ ] If a contact changes his onion address, we get a notification with an option to update the onion address.
- [ ] If a contact connects with an unknown onion address, we generate a notification and disallow the contact to connect to us until the matter is resolved.

## Messages
- [ ] If a message is in sent state, but not confirmed, re-send it next time we are connected to that contact.

## File Transfers p2p
- [ ] Automatically retry transfers that was ongoing or queued when we are connected. The sender side sends a new offer, and the receiver automatically resumes the transfer.
- [ ] Add context menu on failed files to re-try the transfer. If the sender executes a re-try, the file-state changes to queued, and a new offer is sent. If the receiver executes a re-try, the state is changes to offered, and a new ack is sent to start the transfer.
- [ ] Only accept 16 file offers in the queue at any time from one contact (to stop DOS)
- [ ] Time out stalled file transfers on both ends
- [ ] Today we send large parts of large files to the network, much faster then the receiver can receive, efficiently preventing control messages to go trough. We need to slow down, may be by requireing ack's for each n block of data. 

## Log and history
- [ ] The log winows must show the logs in a readable format. 
- [ ] Long-clicking on a line hives the information in a pop-up dialog
- [ ] Show recent history (events) in a new window, sorted by time, grouped on identity.

## Events
- [ ] Message(s) received. Group on identity, contact, date.
- [ ] File sent, received, rejected or failed
- [ ] Contact request sent/received (including current status)
- [ ] Contact added or deleted
- [ ] Identity added or deleted
- [ ] Contact changed nick-name (group on date, show the current)
- [ ] Contact changed avatar (group on date)
- [ ] Contact blocked/unblocked
- [ ] Contact seen (for contacts that have been unavailable for more than a week)

## Tests
- [ ] Add testing framework
- [ ] Add mock framework
- [ ] Create new unit tests for torlib
- [ ] Create new unit tests for protlib
- [ ] Create new unit tests for cryptolib
- [ ] Create new unit tests for corelib
- [ ] Create new unit tests for modelslib
- [ ] Create new unit tests for UI
- [ ] Create new functional tests for corelib
- [ ] Create new functional tests for modelslib (All UI acessible methods)
- [ ] Create new functional tests tests for QML UI

## CI
- [ ] Create CI pipeline for Ubuntu 18.04 LTS
- [ ] Create CI pipeline for Debian 10
- [ ] Create CI pipeline for Windows
- [ ] Create CI pipeline for Macos
- [ ] Create CI pipeline for appimage
- [ ] Create CI pipeline for Android
- [ ] Create CI pipeline for ios

