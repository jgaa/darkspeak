
import QtQuick 2.4
import QtQuick.Controls 1.3

import com.jgaa.darkspeak 1.0

ApplicationWindow {
    id: main_window
    visible: true
    width: 300
    height: 400
    title: qsTr("DarkSpeak")

    toolBar: MainToolBar { id: mainToolBar }

    ScrollView {
        id: main_pane
        x: 0
        height: parent.height
        width: main_window.width * 2

        ContactsListView {
            id: contacts
            width: main_window.width
            x: 0
            height: parent.height
        }

        ChatView {
            id: chat
            x: main_window.width
            height: main_pane.height
            width: main_window.width
        }

        states: [
            State {
                name: "chat"
                PropertyChanges { target: main_pane; x: - main_window.width }
                PropertyChanges { target: mainToolBar; state: "chat"}
            }
        ]

        transitions: [
            Transition {
                from: "*"; to: "*"
                NumberAnimation { properties: "x,y"; easing.type: Easing.InOutQuad; duration: 100 }
            }
        ]
    }
}

