 

# Features

## Identities

**Create identity**

Creates a new identify for a user of the application. 

- [x] Creates a new private and public key.
- [x] Creates a new hidden Tor service.
- [x] Creates an entry in the local identities table in the Sqlite storage.
- [ ] Deny creation of existing name

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
- [ ] Context menu
- [ ] Ask the user to confirm.
- [ ] Import the Tor hidden service via a dialog where the hidden service identifier (onion address), port and private key is provided. Initialize the port field with a valid, random port.
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
- [ ] Show a slider / checkbox that enables / disable auto-connect for the identity
- [ ] Show the avatar image if the user has added one

**Select avatar image**
- [ ] Context menu
- [ ] Provide a file-dialog where the user can select an avatar image. 
- [ ] Scale and crop the image to 120 x 120 pixels and store it in the database.
- [ ] Update the UI with the new avatar image.
- [ ] Notify connected contacts about the change.

**Change nickname**
- [ ] Context menu
- [ ] Provide a dialog where the user can enter a new nickname.
- [ ] Update the database and UI.
- [ ] Notify connected contacts about the change.

**Connect / Disconnect Tor hidden service**
- [x] Context menu. (Show text "Connect" | "Disconnect" depending on state)
- [x] Start connecting or disconnecting depending on current state.
- [x] Start / stop the Tor hidden service for that identity
- [ ] Start a local socket for each identity and use that for the Tor service


## Contacts

**Add a contact**

- [x] Add "Add Contact" icon in the toolbar
- [x] Check the clipboard for a valid contact (name:base58pubkey) and offer to use it if it exists.
- [x] Show a contact dialog with nickname (read only), name, transport, base58pubkkey, notes, Attributes: blocked, Favorite, autoconnect
- [ ] Provide a means to add a message with the addme request
- [ ] For default name, show sequence numbered nickanme if the name is in use
- [ ] Validate input: Name (extistying name), hash from pubkey (existing contact), onion address correctness, handle correctnes. Ony allow to preceed when all calidations are OK.


**Present contacts**
- [v] Show a view similar to the Identities list (verbose)
- [ ] Show a toggle verbose/brief view mode icon in the toolbar
- [ ] Show a shortlist with a smaller avatar, name and blocked / unread messages icon

**Copy contact**
- [ ] Add context menu
- [ ] Copy nickname:base58pubkey to the clipboard

**Connect / disconnect**
- [x] Add context menu
- [ ] Add functionality to establish a connection with the contact

**Autoconnect**
- [ ] If the autoconnect flag is set, connect to this client when we go online

**Block contact**
- [ ] Provide a button in the contact dialog to block the contact
- [ ] If blocked, disallow the client to connect to us.
- [ ] If blocked, disable the connect context menu and the autoconnect feature
- [ ] Show a bloched icon where we usually show the online status in the contact listings

**Online status**
- [ ] Show an overlay icon over the avatar with the online status: contacting, disconnected, offline, connecting, online, rejected
- [ ] Update the status as the Transport layer notifies us about changes

**Last seen**
- [ ] Update a property for the contact as the Transport layer notifies us about changes


**Unread messages**
- [ ] Update a property for the contact if he has unread messages. Do not flag this if the associated messages view is active and have focus

**Remember selected contact for identity**
- [ ] When changing identity, remember the selected contact (by hash), and try to re-select it when we change back to the same identity. 

**Search / filter**
- [ ] Add a serach-bar to find contacts fast, and show only matching contacts.

**Accept an AddMe request**
- [ ] Accept an addmerequest in the UI and add the contact to the appropriate identity. 
- [ ] Add a RSA-1024 signature for the onion address announced in the AddMe requests, using Tor's private key for that onion address.
- [ ] Verify the RSA-1024 signature before showing the addme request in the UI
