import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    id: root

    property color primaryText: "#243447"
    property color secondaryText: "#475569"
    property color labelText: "#64748b"
    property color selectedFill: "#294d70"
    property color selectedBorder: "#79c2f2"

    header: Header {
        whom: identities.current
        text: qsTr("Contacts for ")
    }

    property var states: [qsTr("Pending"), qsTr("Waiting"), qsTr("OK"), qsTr("Rejected"), qsTr("BLOCKED")]
    property var stateColors: ["#aebdca", "#f2c56b", "#72d6a0", "#f28b82", "#f3f6fa"]
    property var onlineColors: ["#79d8c8", "#7aa7e8", "#e9ad63", "#72d6a0"]

    Connections {
        target: contacts
        function onModelReset() {
            console.log("onModelReset");
            if (contacts.rowCount() === 1) {
                // Only one contact, so just select it as the current one
                list.currentIndex = 0;
            } else {
                list.currentIndex = -1;
            }
        }
    }

    ListView {
        id: list
        interactive: true
        model: contacts
        anchors.fill: parent
        highlight: highlightBar

        onCurrentItemChanged: {
            // This is where we synchronize conversations with the current selected Contact
            conversations.setContact(currentItem ? currentItem.cco : null)
        }

        delegate: Item {
            id: itemDelegate
            width: parent.width
            height: 130
            property Contact cco : contact

            Row {
                id: row1
                anchors.fill: parent
                spacing: 8

                Column {
                    padding: 5
                    anchors.verticalCenter: parent.verticalCenter

                    RoundedImage {
                        source: cco ? cco.avatarUrl : ""
                        height: 100
                        width: 100
                        border.width: 8
                        border.color: cco
                                      ? cco.blocked ? "firebrick"
                                      : cco.online ? "lime" : "blue"
                                      : "grey"
                        Rectangle {
                            height: parent.width / 3
                            color: cco ? cco.blocked ? "firebrick" : root.onlineColors[cco.onlineStatus] : "grey"
                            width: height
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 0
                            radius: width*0.5

                            Image {
                                id: torStatus
                                anchors.fill: parent
                                source: cco ? cco.onlineIcon : ""
                            }
                        }
                    }
                }

                Column {
                    spacing: 10
                    x: 116
                    Text {
                        font.pointSize: 14
                        color: itemDelegate.ListView.isCurrentItem ? "#ffffff" : root.primaryText
                        text: cco
                            ? cco.name ? cco.name : cco.nickName
                            : ""
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }

                    GridLayout {
                        rowSpacing: 0
                        rows: 5
                        flow: GridLayout.TopToBottom
                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("Nick")}
                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("Last seen")}
                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("Handle")}
                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("Address")}
                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("Status")}

                        Text {
                            font.pointSize: 9;
                            color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                            text: cco ? cco.nickName : ""
                        }

                        FormattedTime {
                            font.pointSize: 9;
                            color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                            value: cco ? cco.lastSeen : null
                        }

                        Text {
                            font.pointSize: 9;
                            color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                            text: cco ? cco.handle : null
                        }

                        Text {
                            font.pointSize: 9;
                            color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                            text: cco ? cco.address : ""
                        }

                        RowLayout {
                            spacing: 4

                            Text {
                                font.pointSize: 8;
                                color: cco
                                       ? cco.peerVerified ? "lime" : "yellow"
                                       : "grey"
                                text: cco
                                    ? cco.peerVerified ? qsTr("Verified") : qsTr("Unverified")
                                    : ""
                            }

                            Text {
                                color: cco ? root.stateColors[cco.state] : "grey"
                                font.pointSize: 8;
                                text: cco ? root.states[cco.state] : ""
                                Rectangle {
                                    visible: cco && cco.state === Contact.BLOCKED
                                    color: "firebrick"
                                    z: -1
                                    anchors.fill: parent
                                }
                            }

                            Text {
                                color: root.secondaryText
                                font.pointSize: 8;
                                text: cco
                                      ? cco.whoInitiated === Contact.ME ? qsTr("AddedByMe") : ""
                                      : ""

                            }

                            Text {
                                color: cco
                                       ? cco.autoConnect ? "lime" : "orange"
                                       : "grey"
                                font.pointSize: 8;
                                text: cco
                                      ? cco.autoConnect ? qsTr("Auto") : qsTr("Manual")
                                      : ""
                            }

                            Text {
                                color: root.primaryText
                                font.pointSize: 8;
                                text: cco
                                      ? cco.iBlocked ? qsTr(" Blocked by me") : ""
                                      : ""
                                    Rectangle {
                                        color: "firebrick"
                                        z: -1
                                        anchors.fill: parent
                                    }
                            }
                        }
                    }
                }
            }

            MouseArea {
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                anchors.fill: parent
                onClicked: function(mouse) {
                    list.currentIndex = index
                    if (mouse.button === Qt.RightButton) {
                        contextMenu.x = mouse.x;
                        contextMenu.y = mouse.y;
                        contextMenu.open();
                    }
                }

                onPressAndHold: function(mouse) {
                    list.currentIndex = index
                    contextMenu.x = mouse.x;
                    contextMenu.y = mouse.y;
                    contextMenu.open();
                }
            }
        }

        function toggleOnline() {
            if (currentItem.cco.online) {
                currentItem.cco.disconnectFromContact(true);
            } else {
                currentItem.cco.connectToContact();
            }
        }

        function sendMessage() {
            var cv = currentItem.cco.getDefaultConversation();
            conversations.setCurrent(cv);
            manager.currentPage = Manager.CONVERSATIONS
        }

        function editCurrent() {
            var component = Qt.createComponent("qrc:/EditContactDialog.qml")
            var dlg = component.createObject(mainWindow, {"parent" : mainWindow,
                                              "contact" : currentItem.cco});
            dlg.open()
        }

        function copy() {
            manager.textToClipboard(manager.getIdentityText(currentItem.cco.nickName,
                                                            currentItem.cco.b58identity))
        }

        function deleteCurrent() {
            if (currentItem && currentItem.cco) {
                manager.deleteContact(currentItem.cco.uuid)
            }
        }
    }

    Component {
         id: highlightBar
         Rectangle {
             radius: 5
             opacity: 1
             y: list.currentItem ? list.currentItem.y : 0;
             color: root.selectedFill
             border.color: root.selectedBorder
             Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
             visible: list.currentItem
         }
    }

    Menu {
        id: contextMenu

        MenuItem {
            icon.source: "qrc:///images/onion-bw.svg"
            onTriggered: list.toggleOnline()
            enabled: manager.online && list.currentItem && !list.currentItem.cco.iBlocked
            text: (list.currentItem && list.currentItem.cco.online) ? qsTr("Disconnect") : qsTr("Connect")
        }

        MenuItem {
            icon.name: "mail-send"
            enabled: list.currentItem
            onTriggered: list.sendMessage()
            text: qsTr("Send Message")
        }

        MenuItem {
            text: qsTr("Edit")
            icon.name: "insert-text"
            onTriggered: list.editCurrent()
        }

        MenuItem {
            text: qsTr("Copy contact")
            icon.name: "edit-copy"
            onTriggered: list.copy()
        }

        MenuItem {
            text: qsTr("Delete contact")
            icon.name: "edit-delete"
            enabled: list.currentItem && list.currentItem.cco
            onTriggered: confirmDelete.open()
        }
    }

    MessageDialog {
        id: confirmDelete
        title: qsTr("Delete contact")
        text: qsTr("Do you really want to delete this contact?")
        detailedText: qsTr("All conversations, messages, files and notifications for this contact will also be deleted.")
        buttons: MessageDialog.Yes | MessageDialog.Cancel
        onButtonClicked: function(button) {
            if (button === MessageDialog.Yes) {
                list.deleteCurrent()
            }
        }
    }
}

/*##^##
Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
##^##*/
