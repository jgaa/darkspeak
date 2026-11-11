import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    id: root
    width: 600 * appearance.factor
    height: 400 * appearance.factor

    property color primaryText: appearance.text
    property color secondaryText: appearance.secondaryText
    property color identityHeader: appearance.raised

    header: Label {
        textFormat: Text.PlainText
        text: qsTr("Home")
        font.pixelSize: appearance.fontPixels * 2
        padding: 10 * appearance.factor
    }

    Label {
        textFormat: Text.PlainText
        anchors.centerIn: parent
        visible: list.count === 0
        text: qsTr("No new notifications")
        color: appearance.secondaryText
        font: appearance.headingFont
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.margins: appearance.spacing
        clip: true
        model: notifications
        section.property: "identityName"
        section.delegate: Component {
            Rectangle {
                width: list.width
                height: 36 * appearance.factor
                color: root.identityHeader

                Label {
                    textFormat: Text.PlainText
                    anchors.fill: parent
                    leftPadding: 10 * appearance.factor
                    verticalAlignment: Text.AlignVCenter
                    text: section
                    color: appearance.selectedText
                    font.bold: true
                    font.pixelSize: 16 * appearance.factor
                }
            }
        }

        delegate: Item {
            id: notificationItem
            width: list.width
            property var contactObject: contact
            property string avatar: contactObject && contactObject.avatarUrl ? contactObject.avatarUrl : avatarUrl
            property int notificationHeight: type === NotificationsModel.N_ADDME ? 92 : 62
            height: (contactHeader ? 56 : 0) + notificationHeight

            Column {
                anchors.fill: parent
                spacing: 2 * appearance.factor

                RowLayout {
                    visible: contactHeader
                    width: parent.width
                    height: 52 * appearance.factor
                    spacing: 10 * appearance.factor

                    Image {
                        Layout.preferredWidth: 42 * appearance.factor
                        Layout.preferredHeight: 42 * appearance.factor
                        fillMode: Image.PreserveAspectFit
                        source: notificationItem.avatar
                    }

                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: contactName || qsTr("Unknown contact")
                        color: root.primaryText
                        font.bold: true
                        font.pixelSize: 15 * appearance.factor
                    }
                }

                Rectangle {
                    width: parent.width
                    height: notificationItem.notificationHeight
                    color: mouseArea.containsMouse ? appearance.hover : appearance.surface
                    radius: 5 * appearance.factor

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: appearance.spacing
                        spacing: 10 * appearance.factor

                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: type === NotificationsModel.N_ADDME ? qsTr("New contact request") : type === NotificationsModel.N_MESSAGE ? qsTr("Unread messages (%1)").arg(unreadCount) : qsTr("Incoming file: %1").arg(fileName)
                            color: root.primaryText
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            textFormat: Text.PlainText
                            visible: type === NotificationsModel.N_ADDME
                            Layout.maximumWidth: 360 * appearance.factor
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
                            list.currentIndex = index;
                            if (type === NotificationsModel.N_ADDME) {
                                requestLoader.setSource("AcceptContactDialog.qml", {
                                    notificationRow: index,
                                    requesterName: nickName,
                                    requestMessage: message,
                                    requesterAvatarUrl: avatar,
                                    requesterIdentityId: requesterIdentityId,
                                    requestingIdentity: identities.identityFromId(identityId)
                                });
                                requestLoader.active = true;
                            } else {
                                openConversation();
                            }
                        }
                    }
                }
            }

            function openConversation() {
                var identity = identities.identityFromId(identityId);
                if (!identity)
                    return;
                identities.setCurrentIdentityById(identityId);
                if (conversationId > 0)
                    conversations.setConversationById(conversationId);
                else if (contactObject)
                    conversations.setContact(contactObject);
                manager.currentPage = Manager.CONVERSATIONS;

                Qt.callLater(function () {
                    if (conversations.current)
                        conversations.current.unread = 0;
                    notifications.refreshNotifications();
                });
            }
        }
    }

    Loader {
        id: requestLoader
        active: false
        onLoaded: item.open()
        Connections {
            target: requestLoader.item
            function onAccepted() {
                notifications.acceptContact(requestLoader.item.notificationRow, true);
            }
            function onRejected() {
                notifications.acceptContact(requestLoader.item.notificationRow, false);
            }
            function onClosed() {
                requestLoader.active = false;
            }
        }
    }
}
