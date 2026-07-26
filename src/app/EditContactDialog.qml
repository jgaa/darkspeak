import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import com.jgaa.darkspeak 1.0

Dialog {
    id: root
    property Contact contact : null
    property Identity identity: null
    property var clipboardData: null
    standardButtons: Dialog.Ok | Dialog.Cancel


    Component.onCompleted: {
        root.title = identity ? qsTr("Edit Contact") : qsTr("Create a new Contact")

        if (contact) {
            name.text = contact.name
            handle.text = contact.handle
            address.text = contact.address
            addmeMessage.text = contact.addMeMessage
            notes.text = contact.notes
        } else if (clipboardData) {
            name.text = clipboardData.nickName
            handle.text = clipboardData.handle
            address.text = clipboardData.address
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
                placeholderText: clipboardData ? clipboardData.nickName : contact ? contact.nickName : qsTr("Anononymous Coward")
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

        ColumnLayout {
            spacing: 4

            CheckBox {
                id: autoConnect
                text: "AutoConnect"
                checked: contact ? contact.autoConnect : true

            }

            CheckBox {
                id: blocked
                text: "Blocked"
                checked: contact ? contact.blocked : false

            }

            CheckBox {
                Layout.leftMargin: 10
                id: notifyBlocked
                enabled: blocked.checked
                text: "Notify"
                checked: contact ? contact.sendBlockNotice : true
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
            contact.blocked = blocked.checked
            contact.sendBlockNotice = notifyBlocked.checked
        } else {
            const args = {
                "identity" : identity.id,
                "name" : name.text ? name.text : null,
                "nickName" : clipboardData ? clipboardData.nickName : null,
                "handle" : handle.text,
                "address" : address.text,
                "addmeMessage" : addmeMessage.text,
                "autoConnect" : autoConnect.checked,
                "blocked" : blocked.checked,
                "sendBlockNotice" : notifyBlocked.checked,
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
