import QtQuick 2.4
import QtQuick.Controls 1.3

import com.jgaa.darkspeak 1.0

ToolBar {
    width: parent.width
    height: connectButton.height + 8

    ToolButton {
        id: connectButton
        enabled: true
        text: checked ? "Disconnect" : "Connect"
        anchors.left: parent.left
        anchors.leftMargin: 6
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
        //anchors.left: connectButton.left
        x: 0
        id: backButton
        iconSource: "qrc:/images/Back-32.png"
        opacity: 0
        enabled: false
        onClicked: main_pane.state = ""
    }

    ToolButton {
        id: addContactButton
        enabled: true
        text: "Add Contact"
        anchors.top: connectButton.top
        anchors.left: connectButton.right
        anchors.leftMargin: 6
        onClicked: addContactDlg.open()

        AddContactDlg {id: addContactDlg}
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
