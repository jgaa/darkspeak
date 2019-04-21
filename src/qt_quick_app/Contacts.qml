import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.3
import com.jgaa.darkspeak 1.0

Page {
    id: root

    header: Header {
        whom: identities.current
        text: qsTr("Contacts for ")
    }

    property var states: [qsTr("Pending"), qsTr("Waiting"), qsTr("OK"), qsTr("Rejected"), qsTr("BLOCKED")]
    property var stateColors: ["gray", "yellow", "lime", "red", "red"]
    property var onlineColors: ["firebrick", "blue", "orange", "lime"]

    Connections {
        target: contacts
        onModelReset: {
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
                        border.width: 2
                        border.color: cco
                                      ? cco.online ? "lime" : "firebrick"
                                      : "grey"
                        color: "black"

                        Rectangle {
                            height: parent.width / 3
                            color: cco ? root.onlineColors[cco.onlineStatus] : "grey"
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
                        color: "white"
                        text: cco
                            ? cco.name ? cco.name : cco.nickName
                            : ""
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }

                    GridLayout {
                        rowSpacing: 0
                        rows: 5
                        flow: GridLayout.TopToBottom
                        Label { font.pointSize: 9; text: qsTr("Nick")}
                        Label { font.pointSize: 9; text: qsTr("Last seen")}
                        Label { font.pointSize: 9; text: qsTr("Handle")}
                        Label { font.pointSize: 9; text: qsTr("Address")}
                        Label { font.pointSize: 9; text: qsTr("Status")}

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
                            text: cco ? cco.nickName : ""
                        }

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
                            text: cco ? cco.lastSeen : null
                        }

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
                            text: cco ? cco.handle : null
                        }

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
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
                            }

                            Text {
                                color: "skyblue"
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
                        }
                    }
                }
            }

            MouseArea {
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                anchors.fill: parent
                onClicked: {
                    list.currentIndex = index
                    if (mouse.button === Qt.RightButton) {
                        contextMenu.x = mouse.x;
                        contextMenu.y = mouse.y;
                        contextMenu.open();
                    }
                }

                onPressAndHold: {
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
            manager.currentPage = Manager.CHAT
        }

        function editCurrent() {
            var component = Qt.createComponent("qrc:/EditContactDialog.qml")
            var dlg = component.createObject(mainWindow, {"parent" : mainWindow,
                                              "contact" : currentItem.cco});
            dlg.open()
        }
    }

    Component {
         id: highlightBar
         Rectangle {
             radius: 5
             y: list.currentItem ? list.currentItem.y : 0;
             color: "midnightblue"
             border.color: "aquamarine"
             Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
             visible: list.currentItem
         }
    }

    Menu {
        id: contextMenu

        MenuItem {
            icon.source: "qrc:///images/onion-bw.svg"
            onTriggered: list.toggleOnline()
            enabled: manager.online
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
    }
}
