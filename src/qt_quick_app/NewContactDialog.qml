import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Dialog {
    id: root
    title: qsTr("Add Contact")
    standardButtons: StandardButton.Apply | StandardButton.Cancel
    property string nickName: qsTr("Anonymous Coward")
    property string handle: ""
    property string address: ""

    ColumnLayout {
        spacing: 4
        Layout.fillWidth: parent.width

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 4
            Layout.fillWidth: parent.width
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Name")}
            Label { font.pointSize: 9; text: qsTr("Handle")}
            Label { font.pointSize: 9; text: qsTr("Onion")}
            Label { font.pointSize: 9; text: qsTr("Message")}

            TextField {
                id: name
                Layout.fillWidth: true
                placeholderText: root.nickName
            }

            TextField {
                id: handle
                Layout.fillWidth: true
                text: root.handle
            }

            TextField {
                id: onion
                Layout.fillWidth: true
                text: root.address
            }

            TextField {
                id: addmeMessage
                Layout.fillWidth: true
                placeholderText: qsTr("Please add me")
            }
        }

        RowLayout {
            spacing: 4

            CheckBox {
                id: autoConnect
                text: "AutoConnect"
                checked: true

            }
        }

        Label { font.pointSize: 9; text: qsTr("Notes")}

        TextArea {
            id: notes
            Layout.fillHeight: true
            Layout.minimumHeight: 30
            Layout.minimumWidth: 3
            Layout.fillWidth: true
        }
    }

    Connections {
        target: root
        onClicked: print("clicked")
    }

    onApply: {
        contacts.createContact(root.nickName,
                               name.text,
                               handle.text,
                               onion.text,
                               addmeMessage.text,
                               notes.text,
                               autoConnect.checked)
        close()
    }
}
