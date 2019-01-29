import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.3
import com.jgaa.darkspeak 1.0

Page {

    header: Label {
        text: qsTr("Contacts")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }

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
        property bool isOnline : model.getOnlineStatus(currentIndex) === ContactsModel.ONLINE

        delegate: Item {
            id: itemDelegate
            width: parent.width
            height: 130

            Row {
                id: row1
                anchors.fill: parent
                spacing: 8

                Rectangle {
                    id: avatarFrame
                    height: 100
                    width: 100
                    radius: 5
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    border.color: online === ContactsModel.ONLINE ? "lime" : "firebrick"
                    color: "black"

                    Image {
                        id: avatar
                        fillMode: Image.PreserveAspectFit
                        height: 96
                        width: 96
                        x: 2
                        y: 2
                        source: avatarImage

                        Rectangle {
                            height: parent.width / 3
                            color: online === ContactsModel.DISCONNECTED ? "firebrick"
                                 : online === ContactsModel.OFFLINE ? "blue"
                                 : online === ContactsModel.CONNECTING ? "orange"
                                 : online === ContactsModel.ONLINE ? "lime" : "red"
                            width: height
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 0
                            radius: width*0.5

                            Image {
                                id: torStatus
                                anchors.fill: parent
                                source: onlineIcon
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
                        text: name ? name : nickName
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }

                    GridLayout {
                        rowSpacing: 0
                        rows: 5
                        flow: GridLayout.TopToBottom
                        Label { font.pointSize: 9; text: qsTr("Nick")}
                        Label { font.pointSize: 9; text: qsTr("Connected")}
                        Label { font.pointSize: 9; text: qsTr("handle")}
                        Label { font.pointSize: 9; text: qsTr("Onion")}
                        Label { font.pointSize: 9; text: qsTr("Status")}

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
                            text: nickName
                        }

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
                            text: created
                        }

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
                            text: handle
                        }

                        Text {
                            font.pointSize: 9;
                            color: "skyblue"
                            text: onion
                        }

                        Row {
                            anchors.fill: parent
                            spacing: 4
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
            if (isOnline) {
                model.disconnectTransport(currentIndex);
            } else {
                model.connectTransport(currentIndex);
            }
        }
    }

    Component {
         id: highlightBar
         Rectangle {
             radius: 5
             y: list.currentItem.y;
             color: "midnightblue"
             border.color: "aquamarine"
             Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
         }
    }

    Menu {
        id: contextMenu

        onAboutToShow: prepare()

        function prepare() {
            online.text =  list.isOnline ? qsTr("Disconnect") : qsTr("Connect")
        }

        MenuItem {
            id: online
            icon.source: "qrc:///images/onion-bw.svg"
            onTriggered: list.toggleOnline()
            enabled: manager.online
        }
    }
}
