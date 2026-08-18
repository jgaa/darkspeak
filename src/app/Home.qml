import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    id: root
    width: 600
    height: 400

    property color primaryText: "#243447"
    property color secondaryText: "#475569"
    property color identityHeader: "#263746"

    header: Label {
        text: qsTr("Home")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }

    Label {
        anchors.centerIn: parent
        visible: list.count === 0
        text: qsTr("No new notifications")
        color: "#4b5563"
        font.pixelSize: 18
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.margins: 8
        clip: true
        model: notifications
        section.property: "identityName"
        section.delegate: Component {
            Rectangle {
                width: list.width
                height: 36
                color: root.identityHeader

                Label {
                    anchors.fill: parent
                    leftPadding: 10
                    verticalAlignment: Text.AlignVCenter
                    text: section
                    color: "#ffffff"
                    font.bold: true
                    font.pixelSize: 16
                }
            }
        }

        delegate: Item {
            id: notificationItem
            width: list.width
            property var contactObject: contact
            property string avatar: contactObject && contactObject.avatarUrl
                                   ? contactObject.avatarUrl : avatarUrl
            property int notificationHeight: type === NotificationsModel.N_ADDME ? 92 : 62
            height: (contactHeader ? 56 : 0) + notificationHeight

            Column {
                anchors.fill: parent
                spacing: 2

                RowLayout {
                    visible: contactHeader
                    width: parent.width
                    height: 52
                    spacing: 10

                    Image {
                        Layout.preferredWidth: 42
                        Layout.preferredHeight: 42
                        fillMode: Image.PreserveAspectFit
                        source: notificationItem.avatar
                    }

                    Label {
                        Layout.fillWidth: true
                        text: contactName || qsTr("Unknown contact")
                        color: root.primaryText
                        font.bold: true
                        font.pixelSize: 15
                    }
                }

                Rectangle {
                    width: parent.width
                    height: notificationItem.notificationHeight
                    color: mouseArea.containsMouse ? "#e5eef8" : "#f7f9fc"
                    radius: 5

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 10

                        Label {
                            Layout.fillWidth: true
                            text: type === NotificationsModel.N_ADDME
                                  ? qsTr("New contact request")
                                  : type === NotificationsModel.N_MESSAGE
                                    ? qsTr("Unread messages (%1)").arg(unreadCount)
                                    : qsTr("Incoming file: %1").arg(fileName)
                            color: root.primaryText
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            visible: type === NotificationsModel.N_ADDME
                            Layout.maximumWidth: 360
                            text: message
                            color: root.secondaryText
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            list.currentIndex = index
                            if (type === NotificationsModel.N_ADDME) {
                                confirmNewContact.notificationRow = index
                                confirmNewContact.requesterName = nickName
                                confirmNewContact.requestMessage = message
                                confirmNewContact.requesterAvatarUrl = avatar
                                confirmNewContact.requesterIdentityId = requesterIdentityId
                                confirmNewContact.requestingIdentity = identities.identityFromId(identityId)
                                confirmNewContact.open()
                            } else {
                                openConversation()
                            }
                        }
                    }
                }
            }

            function openConversation() {
                var identity = identities.identityFromId(identityId)
                if (!identity)
                    return

                identities.setCurrentIdentityById(identityId)
                contacts.setIdentity(identity.uuid)
                conversations.setIdentity(identity.uuid)
                if (conversationId > 0)
                    conversations.setConversationById(conversationId)
                else if (contactObject)
                    conversations.setContact(contactObject)
                manager.currentPage = Manager.CONVERSATIONS

                Qt.callLater(function() {
                    if (conversations.current)
                        conversations.current.unread = 0
                    notifications.refreshNotifications()
                })
            }
        }
    }

    AcceptContactDialog {
        id: confirmNewContact
        onAccepted: notifications.acceptContact(notificationRow, true)
        onRejected: notifications.acceptContact(notificationRow, false)
    }
}
