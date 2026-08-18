import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Dialog {
    id: root
    property Contact contact : null
    property Identity identity: null
    property var clipboardData: null
    width: Math.min(Math.max(mainWindow.width - 80, 560), 800)
    height: Math.min(Math.max(mainWindow.height - 80, 460), 650)
    anchors.centerIn: parent
    padding: 16
    standardButtons: identityId.text.length > 0 ? Dialog.Ok | Dialog.Cancel : Dialog.Cancel


    Component.onCompleted: {
        root.title = identity ? qsTr("Edit Contact") : qsTr("Create a new Contact")

        if (contact) {
            name.text = contact.name
            identityId.text = contact.b58identity
            addmeMessage.text = contact.addMeMessage
            notes.text = contact.notes
        } else if (clipboardData) {
            name.text = clipboardData.nickName
            identityId.text = clipboardData.identity
            if (identity && identityId.text.length > 0) {
                Qt.callLater(function() { identity.probeContact(identityId.text) })
            }
        }

    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("Contact") }
            TabButton {
                text: qsTr("Bio")
                enabled: (contact && contact.hasBio) || (identity && identity.probeBio.length > 0)
            }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 320

            Item {
                ColumnLayout {
                    anchors.fill: parent

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 3
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Name (optional)")}
            Label { font.pointSize: 9; text: qsTr("Identity ID")}
            Label { font.pointSize: 9; text: qsTr("Message")}

            TextField {
                id: name
                placeholderText: clipboardData ? clipboardData.nickName : contact ? contact.nickName : qsTr("Anononymous Coward")
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: identityId
                    Layout.fillWidth: true
                    placeholderText: qsTr("Paste the identity ID")
                    enabled: contact === null
                    onEditingFinished: {
                        if (identity && text.length > 0 && contact === null) {
                            identity.probeContact(text)
                        }
                    }
                }
                Button {
                    text: qsTr("Get public bio")
                    enabled: identity && identityId.text.length > 0 && contact === null
                    onClicked: identity.probeContact(identityId.text)
                }
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
            }

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    Label {
                        visible: !contact && identity && identity.probeBio.length > 0
                        text: qsTr("This is not currently a contact")
                        color: "#4b5563"
                        Layout.fillWidth: true
                    }

                    RoundedImage {
                        id: bioAvatar
                        visible: contact ? contact.hasAvatar : identity && identity.hasProbeAvatar
                        source: contact ? contact.avatarUrl : "image://temp/image"
                        Layout.preferredWidth: 128
                        Layout.preferredHeight: 128
                        border.width: 4
                        border.color: "gray"
                        color: "transparent"
                    }

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        TextEdit {
                            width: root.width - 32
                            textFormat: TextEdit.MarkdownText
                            text: contact ? manager.sanitizeBio(contact.bio)
                                           : identity ? manager.sanitizeBio(identity.probeBio) : ""
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            onLinkActivated: function(link) {
                                manager.textToClipboard(link)
                                manager.setStatusMessage(qsTr("Bio link copied to clipboard"))
                            }
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: identity
        function onProbeAvatarChanged() {
            if (identity && identity.hasProbeAvatar) {
                manager.setTmpImageFromImage(identity.probeAvatar)
                bioAvatar.source = ""
                bioAvatar.source = "image://temp/image"
            }
        }
    }

    onAccepted: {

        if (contact) {
            contact.name = name.text
            contact.addMeMessage = addmeMessage.text
            contact.autoConnect = autoConnect.checked
            contact.notes = notes.text
            contact.blocked = blocked.checked
            contact.sendBlockNotice = notifyBlocked.checked
        } else {
            const args = {
                "identity" : identity.id,
                "name" : name.text ? name.text : null,
                "identityId" : identityId.text,
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
