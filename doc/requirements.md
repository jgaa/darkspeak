 

# Features

## Identities

**Create identity**

Creates a new identify for a user of the application. 

- [x] Creates a new private and public key.
- [x] Creates a new hidden Tor service.
- [x] Creates an entry in the local identities table in the Sqlite storage.

**Delete an identity**
- [ ] Confirm this via input dialog
- [ ] Delete all contacts associated with this identity
- [ ] Delete all conversations for this identity
- [ ] If there are files, ask if they should be deleted as well, and delete them if conformed. 

**Export an identity**
- [ ] Let the user select a file name and destination directory for the export
- [ ] Export the keys and let the user choose if he wants to export the Tor hidden service and the associated contacts. 

**Import an identity**
- [ ] Import from export-file (simple json format, matching the Identity data object and an array of contacts). We import only the identity data and contacts information, no conversations).
- [ ] If no hidden service was exported, create a new hidden service. 

**Create a new Tor hidden service**
- [ ] Ask the user to confirm.
- [ ] Create a new Tor hidden service and replace it with the old one. If the old one was on-line, shut it down before deleting the key. 

**Import a Tor hidden service**
- [ ] Ask the user to confirm.
- [ ] Import the Tor hidden service via a dialog where the hidden service identifier (onion address), port and private key is provided. Initialize the port field with a valid, random port.
- [ ] Allow the user to import a file created from the output of eschalot directly. Select a random port.

**Copy identity as Base58 data.**
- [ ] Create a binary array with the public key and the onion address and the port, encode it to base58check, and copy it to the clipboard. Prefix the data with the users nickname. 
- [ ] Same as above, but save as a file. Let the user select file-name and directory.

**Copy the identity as json**
- [ ] Create a binary array with the public key and encode it to base58check. Compose a json payload with the nickname, pubkey, onion address and port. Copy it to the clipboard.
- [ ] Same as above, but save as a file. Let the user select file-name and directory.

**Display identity in the UI**
- [ ] Show the nickname.
- [ ] Show the base58check encoded pubkey
- [ ] Show the Onion address and port
- [ ] Show online-status for the Tor hidden service
- [ ] Show a button that either start or stop the service
- [ ] Show a slider / checkbox that enables / disable auto-connect for the identity


