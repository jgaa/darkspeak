import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Dialog {
    id: root
    property Contact contact : null
    property Identity identity: null
    property var clip: null
    standardButtons: StandardButton.Ok | StandardButton.Cancel

    Component.onCompleted: {
        if (clip) {
            name.text = clip.nickName
            handle.text = clip.handle
            address.text = clip.address
        }

    }

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
            Label { font.pointSize: 9; text: qsTr("address")}
            Label { font.pointSize: 9; text: qsTr("Message")}

            TextField {
                id: name
                placeholderText: qsTr("Anononymous Coward")
                Layout.fillWidth: true
            }

            TextField {
                id: handle
                Layout.fillWidth: true
                text: root.handle
            }

            TextField {
                id: address
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

    onAccepted: {

        const args = {
            "identity" : identity.id,
            "name" : name.text ? name.text : qsTr("Anononymous Coward"),
            "nickName" : clip ? clip.nickName : null,
            "handle" : handle.text,
            "address" : address.text,
            "addmeMessage" : addmeMessage.text,
            "autoConnect" : autoConnect.checked,
            "notes" : notes.text

        }

        identity.addContact(args)
//        contacts.createContact(root.nickName,
//                               name.text,
//                               handle.text,
//                               address.text,
//                               addmeMessage.text,
//                               notes.text,
//                               autoConnect.checked)
        close()
        //destroy()
    }

    onRejected: {
        destroy();
    }
}
