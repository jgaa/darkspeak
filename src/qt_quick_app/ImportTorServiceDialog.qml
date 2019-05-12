import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Dialog {
    id: root
    property Identity identity: null
    standardButtons: (port.acceptableInput && address.acceptableInput)
                     ? (StandardButton.Ok | StandardButton.Cancel)
                     : StandardButton.Cancel
    title: qsTr("Import service for") + " " + (identity ? identity.name : "")


    ColumnLayout {
        spacing: 4
        anchors.fill: parent

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 2
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("address")}
            Label { font.pointSize: 9; text: qsTr("port")}

            TextField {
                id: address
                Layout.fillWidth: true
                validator: RegExpValidator {
                    regExp: /[a-z2-7]{16}(\.onion)?/
                }
                textColor: acceptableInput ? "black" : "red";
            }

            TextField {
                id: port
                text: Math.floor(Math.random() * 40000) + 10254
                validator: IntValidator {
                    bottom: 1024
                    top: 65535
                }
                textColor: acceptableInput ? "black" : "red";
            }
        }

        Label { font.pointSize: 9; text: qsTr("Private key")}

        TextArea {
            id: key
            Layout.fillHeight: true
            Layout.minimumHeight: 30
            Layout.minimumWidth: 3
            Layout.fillWidth: true
        }
    }

    onAccepted: {
        identity.setNewTorService(address.text, port.text, key.text)
        destroy()
    }

    onRejected: {
        destroy();
    }
}
