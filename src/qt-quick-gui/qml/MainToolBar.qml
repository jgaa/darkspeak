import QtQuick 2.4
import QtQuick.Controls 1.3

import com.jgaa.darkspeak 1.0

ToolBar {
    id: root
    width: parent.width
    height: connectButton.height + 8

    ToolButton {
        x: 0
        id: backButton
        iconSource: "qrc:/images/Back-32.png"
        opacity: 0
        enabled: false
        onClicked: main_pane.state = ""
    }

    ToolButton {
        id: connectButton
        enabled: true
        text: checked ? "Disconnect" : "Connect"
        x: 0
        tooltip: "Connect to the Tor Network"
        onClicked: {
             console.log("Online status is " + contactsModel.onlineStatus)
            if (contactsModel.onlineStatus === ContactsModel.OS_OFF_LINE) {
                darkRoot.goOnline()
            }
            else if (contactsModel.onlineStatus === ContactsModel.OS_CONNECTING) {
                darkRoot.goOffline()
            }
            else if (contactsModel.onlineStatus === ContactsModel.OS_ONLINE) {
                darkRoot.goOffline()
            }
            else if (contactsModel.onlineStatus === ContactsModel.OS_DISCONNECTING) {
                darkRoot.goOnline()
            }
        }
        iconSource: contactsModel.onlineStatusIcon
    }

    ToolButton {
        id: addContactButton
        tooltip: "Add a new contact"
        iconSource: "qrc:/images/AddContact-32.png"
        enabled: true
        text: "Add Contact"
        anchors.top: connectButton.top
        anchors.left: connectButton.right
        anchors.leftMargin: 6
        onClicked: addContactDlg.open()
        AddContactDlg {id: addContactDlg}
    }

    ToolButton {
        id: settingsButton
        tooltip: "Settings"
        iconSource: "qrc:/images/Settings-32.png"
        enabled: true
        text: "Settings"
        anchors.top: addContactButton.top
        x: root.width - width - 6
        onClicked: settingsDlg.open()
        SettingsDlg {
            id: settingsDlg
            settings: darkRoot.settings()
        }
    }


//     Text {
//         text: "w=" + main_window,width
//     }

    states: [
        State {
            name: "chat"
            PropertyChanges { target: connectButton; opacity: 0}
            PropertyChanges { target: connectButton; enabled: false}
            PropertyChanges { target: addContactButton; opacity: 0}
            PropertyChanges { target: addContactButton; enabled: false}
            PropertyChanges { target: backButton; opacity: 1}
            PropertyChanges { target: backButton; enabled: true}
        }
    ]

    transitions: [
        Transition {
            from: "*"; to: "*"
            NumberAnimation { properties: "opacity"; easing.type: Easing.InOutQuad; duration: 200 }
        }
    ]
}
