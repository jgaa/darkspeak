# Building darkspeak on kubuntu 16.10 in a virtual machine

## Get ubuntu ready in a VM

Install kubuntu 16.10. I installed it in a Virtual Box VM with 6 GB ram, 42 GB disk and NAT.
Personally I never use pre-built VM images downloaded from the net. I always install Linux from verified iso images.

Run the commands below to complete the installation.

```sh
$ sudo apt-get update
$ sudo apt-get upgrade
$ sudo apt-get install virtualbox-guest-utils virtualbox-guest-x11 virtualbox-guest-dkms

```
Reboot

## Set up a hidden service for the chat client

```sh
$ sudo apt-get install tor
$ sudo sh -c 'echo "HiddenServiceDir /var/lib/tor/darkspeak
HiddenServicePort 11009 127.0.0.1:11009" >> /etc/tor/torrc'
$ sudo systemctl restart tor
$ sudo cat /var/lib/tor/darkspeak/hostname
```
The last command will show you your hidden service address. This address, without the ".onion" ending is your "Tor Chat ID" on this machine.

## Build and start darkspeak

```sh
$ sudo apt-get install git cmake g++ libboost-all-dev libssl-dev doxygen graphviz qt5-default qttools5-dev-tools qtdeclarative5-dev libqtgconf-dev qml-module-qtquick-extras qtdeclarative5-qtquick2-plugin libqt5quick5 qml-module-qtquick-controls qml-module-qtquick-dialogs qml-module-qtquick-layouts qml-module-qtquick-layouts qml-module-qtquick-particles2 qml-module-qtquick-particles2 qml-module-qtquick-window2 qml-module-qtquick-window2 qttools5-dev-tools qml-module-qtpositioning

$ git clone https://github.com/jgaa/darkspeak.git
$ cd darkspeak
$ git submodule init
$ git submodule update
$ mkdir build
$ cd build
$ cmake ..
$ make
$ sudo cp src/qt-quick-gui/darkspeak-gui /usr/local/bin
$ darkspeak-gui
```
You can now add darkspeak to your KDE start-menu, or just type darkspeak-gui into the start-menu whenever you want to run the application.

When darkspeak is running, open the settings dialog, select the "You" tab, and paste your "Tor Chat ID" into the "Chat ID" field. Then press the Save button and you are ready to press the "Connect to the Tor Network" icon in the main application window.

Note that you may not succeed in contacting Tor Chat users if they run the legacy Tor Chat client under Windows, using the obsolete Tor bundle installed by this client. As far as I have been able to deduce, this is caused by a total lack of connectivity in the Tor network between these old Tor bundles, and an up to date (secure) Tor bundle. The prescription above will use the Tor server available as an ubuntu package as part of the ubuntu distribution. This is the safest option, as tor will be updated along with all other system applications when you update ubuntu.

