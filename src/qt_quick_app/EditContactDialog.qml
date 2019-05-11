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
        root.title = identity ? qsTr("Edit Contact") : qsTr("Create a new Contact")

        if (contact) {
            name.text = contact.name
            handle.text = contact.handle
            address.text = contact.address
            addmeMessage.text = contact.addMeMessage
            notes.text = contact.notes
        } else if (clip) {
            name.text = clip.nickName
            handle.text = clip.handle
            address.text = clip.address
        }

    }

    ColumnLayout {
        spacing: 4

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 4
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Name")}
            Label { font.pointSize: 9; text: qsTr("Handle")}
            Label { font.pointSize: 9; text: qsTr("address")}
            Label { font.pointSize: 9; text: qsTr("Message")}

            TextField {
                id: name
                placeholderText: clip ? clip.nickName : contact ? contact.nickName : qsTr("Anononymous Coward")
                Layout.fillWidth: true
            }

            TextField {
                id: handle
                Layout.fillWidth: true
                text: root.handle
                enabled: contact === null
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
                enabled: contact === null ||
                         (contact.state === Contact.WAITING_FOR_ACCEPTANCE && contact.whoInitiated == Contact.ME)
            }
        }

        RowLayout {
            spacing: 4

            CheckBox {
                id: autoConnect
                text: "AutoConnect"
                checked: contact ? contact.autoConnect : true

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

        if (contact) {
            contact.name = name.text
            contact.address = address.text
            contact.addMeMessage = addmeMessage.text
            contact.autoConnect = autoConnect.checked
            contact.notes = notes.text
        } else {
            const args = {
                "identity" : identity.id,
                "name" : name.text ? name.text : null,
                "nickName" : clip ? clip.nickName : nullptr,
                "handle" : handle.text,
                "address" : address.text,
                "addmeMessage" : addmeMessage.text,
                "autoConnect" : autoConnect.checked,
                "notes" : notes.text
            }

            identity.addContact(args)
        }
        close()
    }

    onRejected: {
        destroy();
    }
}
