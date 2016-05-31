import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Layouts 1.2
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Item {
    id: root

    property alias incomingHost: incomingHostId.text
    property alias incomingPort: incomingPortId.text
    property alias outgoingHost: outgoingHostId.text
    property alias outgoingPort: outgoingPortId.text


    ScrollView {
        id: scroller
        anchors.fill: parent

        ColumnLayout {
            spacing: 8
            Layout.fillWidth: true

            Item { Layout.preferredHeight: 4 } // padding
            Label {text: "Incoming endpoint for Tor" }
            Item { Layout.preferredHeight: 4 } // padding

            RowLayout {
                id: incomingHostRow
                spacing: 6
                Label { text: "Host or IP:" }
                TextField {
                    id: incomingHostId
                    text: settings.torIncomingHost
                    Layout.alignment: Qt.AlignBaseline
                    Layout.fillWidth: true
                }
            }

            RowLayout {
                id: incomingPortRosRow
                spacing: 6
                Label { text: "Port:" }
                TextField {
                    id: incomingPortId
                    text: settings.torIncomingPort
                    Layout.alignment: Qt.AlignBaseline
                    Layout.fillWidth: true
                }
            }

            Item { Layout.preferredHeight: 4 } // padding
            Label {text: "Outgoing Tor Server (Socks5)" }
            Item { Layout.preferredHeight: 4 } // padding

            RowLayout {
                id: outgoingHostRow
                spacing: 6
                Label { text: "Host or IP:" }
                TextField {
                    id: outgoingHostId
                    text: settings.torOutgoingHost
                    Layout.alignment: Qt.AlignBaseline
                    Layout.fillWidth: true
                }
            }

            RowLayout {
                id: outgoingPortRosRow
                spacing: 6
                Label { text: "Port:" }
                TextField {
                    id: outgoingPortId
                    text: settings.torOutgoingPort
                    Layout.alignment: Qt.AlignBaseline
                    Layout.fillWidth: true
                }
            }

            Item { Layout.preferredHeight: 4 } // padding
            Label {text: "Takes effect when you go on-line" }
        }
    }
}
