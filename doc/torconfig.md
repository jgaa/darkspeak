# Tor configuration.

In order to use darkspeak with an external Tor server,
you must enable the control-interface on the Tor server,
and set up authentication.

## Debian and Ubuntu

Install Tor and add yourself to the tor user-group

```sh
sudo apt-get install tor
sudo adduser $USER debian-tor
```

Then edit torrc

```sh
sudo vi /etc/tor/torrc
```

Uncomment ControlPort and CookieAuthentication

```
## The port on which Tor will listen for local connections from Tor
## controller applications, as documented in control-spec.txt.
ControlPort 9051
## If you enable the controlport, be sure to enable one of these
## authentication methods, to prevent attackers from accessing it.
#HashedControlPassword 16:872860B76453A77D60CA2BB8C1A7042072093276A3D701AD684053EC4C
CookieAuthentication 1
```

Restart tor, 

```
sudo systemctl restart tor
```

log out and log back in.

Now you should be able to use the system's tor-server from darkspeak.

DarkSpeak accepts Tor v3 services only. The canonical peer address is a
56-character lowercase base32 identifier with the `.onion` suffix and an
explicit port, for example
`abcdefghijklmnopqrstuv234567abcdefghijklmnopqrstuv234567.onion:443` (the
identifier must still pass Tor's v3 checksum validation). Legacy 16-character
v2 addresses are rejected.

The control endpoint and SOCKS endpoint are configured independently. The
defaults are control `127.0.0.1:9051` and SOCKS `127.0.0.1:9050`; the local
application listener host is configured separately.
