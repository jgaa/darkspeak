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

