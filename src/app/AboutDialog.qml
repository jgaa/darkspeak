import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

UiDialog {
    id: root
    anchors.centerIn: parent
    modal: true
    focus: true
    title: qsTr("About DarkSpeak")
    standardButtons: Dialog.Ok
    width: Math.min(mainWindow.width - 24 * appearance.factor, 560 * appearance.factor)
    height: Math.min(mainWindow.height - 24 * appearance.factor, 480 * appearance.factor)
    padding: 20 * appearance.factor

    contentItem: ScrollView {
        clip: true
        Column {
            width: parent.width
            spacing: appearance.largeSpacing

            Label {
                textFormat: Text.PlainText
                width: parent.width
                text: qsTr("DarkSpeak is a secure peer-to-peer messenger using the Tor network for private communication.")
                wrapMode: Text.WordWrap
            }

            GridLayout {
                width: parent.width
                columns: 2
                columnSpacing: 18 * appearance.factor
                rowSpacing: appearance.spacing

                Label {
                    textFormat: Text.PlainText
                    text: qsTr("Application")
                }
                Label {
                    textFormat: Text.PlainText
                    text: manager ? (manager.programNameAndVersion || "") : ""
                }

                Label {
                    textFormat: Text.PlainText
                    text: qsTr("Qt version")
                }
                Label {
                    textFormat: Text.PlainText
                    text: manager.qtVersion
                }

                Label {
                    textFormat: Text.PlainText
                    text: qsTr("Tor version")
                }
                Label {
                    textFormat: Text.PlainText
                    text: manager.torVersion || qsTr("Not connected")
                }

                Label {
                    textFormat: Text.PlainText
                    text: qsTr("Compiler")
                }
                Label {
                    textFormat: Text.PlainText
                    text: manager.compiler
                }

                Label {
                    textFormat: Text.PlainText
                    text: qsTr("Compiled")
                }
                Label {
                    textFormat: Text.PlainText
                    text: manager.buildDate
                }
            }

            Label {
                width: parent.width
                text: qsTr("Web page: <a href=\"https://darkspeak.org/\">https://darkspeak.org/</a>")
                textFormat: Text.RichText
                onLinkActivated: function (link) {
                    Qt.openUrlExternally(link);
                }
            }

            Label {
                width: parent.width
                text: qsTr("GitHub: <a href=\"https://github.com/jgaa/darkspeak\">https://github.com/jgaa/darkspeak</a>")
                textFormat: Text.RichText
                onLinkActivated: function (link) {
                    Qt.openUrlExternally(link);
                }
            }
        }
    }
}
