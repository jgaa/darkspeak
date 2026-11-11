import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

UiDialog {
    id: root
    property Identity identity: null
    standardButtons: (port.acceptableInput && address.acceptableInput) ? (Dialog.Ok | Dialog.Cancel) : Dialog.Cancel
    title: qsTr("Import service for %1").arg(identity ? identity.name : "")
    width: Math.min(mainWindow.width - 24 * appearance.factor, 600 * appearance.factor)
    height: Math.min(mainWindow.height - 24 * appearance.factor, 420 * appearance.factor)

    ColumnLayout {
        spacing: appearance.smallSpacing
        anchors.fill: parent

        GridLayout {
            id: fields
            rowSpacing: appearance.smallSpacing
            rows: 2
            flow: GridLayout.TopToBottom

            Label {
                textFormat: Text.PlainText
                font: appearance.captionFont
                text: qsTr("address")
            }
            Label {
                textFormat: Text.PlainText
                font: appearance.captionFont
                text: qsTr("port")
            }

            TextField {
                id: address
                Layout.fillWidth: true
                validator: RegularExpressionValidator {
                    regularExpression: /[a-z2-7]{56}(\.onion)?/
                }
                color: acceptableInput ? appearance.text : appearance.error
            }

            TextField {
                id: port
                text: Math.floor(Math.random() * 40000) + 10254
                validator: IntValidator {
                    bottom: 1024
                    top: 65535
                }
                color: acceptableInput ? appearance.text : appearance.error
            }
        }

        Label {
            textFormat: Text.PlainText
            font: appearance.captionFont
            text: qsTr("Private key")
        }

        TextArea {
            id: key
            Layout.fillHeight: true
            Layout.minimumHeight: 30 * appearance.factor
            Layout.minimumWidth: 3 * appearance.factor
            Layout.fillWidth: true
        }
    }

    onAccepted: {
        identity.setNewTorService(address.text, port.text, key.text);
        destroy();
    }

    onRejected: {
        destroy();
    }
}
