import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Dialog {
    id: root
    anchors.centerIn: parent
    modal: true
    focus: true
    title: qsTr("About DarkSpeak")
    standardButtons: Dialog.Ok
    width: Math.min(parent ? parent.width - 80 : 560, 560)
    height: Math.min(parent ? parent.height - 80 : 480, 480)
    padding: 20

    contentItem: ScrollView {
        clip: true
        Column {
            width: parent.width
            spacing: 16

            Label {
                width: parent.width
                text: qsTr("DarkSpeak is a secure peer-to-peer messenger using the Tor network for private communication.")
                wrapMode: Text.WordWrap
            }

            GridLayout {
                width: parent.width
                columns: 2
                columnSpacing: 18
                rowSpacing: 8

                Label { text: qsTr("Application") }
                Label { text: manager ? (manager.programNameAndVersion || "") : "" }

                Label { text: qsTr("Qt version") }
                Label { text: Qt.version }

                Label { text: qsTr("Tor version") }
                Label { text: manager.torVersion || qsTr("Not connected") }
            }
        }
    }
}
