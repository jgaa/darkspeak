import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.3
import com.jgaa.darkspeak 1.0


Page {
    width: 600
    height: 400

    header: Header {
        whom: identities.current
        text: qsTr("Conversations for ")
    }

    Connections {
        target: conversations
        onModelReset: {
            console.log("onModelReset");
            if (conversations.rowCount() === 1) {
                // Only one contact, so just select it as the current one
                list.currentIndex = 0;
            } else {
                list.currentIndex = -1;
            }
        }

        onCurrentRowChanged: {
            list.currentIndex = conversations.currentRow
            messages.setConversation(conversations.current)
        }
    }

    ListView {
        id: list
        interactive: true
        model: conversations
        anchors.fill: parent
        highlight: highlightBar

        onCurrentIndexChanged: {
            conversations.currentRow = list.currentIndex
        }

        delegate: Item {
            id: itemDelegate
            width: parent.width
            height: 60
            property Conversation cco : conversation

            Row {
                id: row1
                anchors.fill: parent
                spacing: 4

                Column {
                    padding: 5
                    anchors.verticalCenter: parent.verticalCenter

                    RoundedImage {
                        source: cco ? cco.participant.avatarUrl : ""
                        height: 45
                        width: 45
                        border.width: 2
                        border.color: cco
                                      ? cco.participant.online ? "lime" : "red"
                                      : "grey"
                        color: "black"
                    }
                }

                Column {
                    spacing: 10

                    Text {
                        font.pointSize: 10
                        color: "white"
                        text: cco
                              ? cco.name
                                ? cco.name : cco.participant.name
                                   ? cco.participant.name

                                   : cco.participant.nickName
                              : ""
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }

                    Text {
                        font.pointSize: 14
                        color: "skyblue"
                        text: cco ? cco.topic : ""
                        font.bold: itemDelegate.ListView.isCurrentItem
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

    }

    Component {
         id: highlightBar
         Rectangle {
             radius: 5
             y: list.currentItem ? list.currentItem.y : 0;
             color: "midnightblue"
             border.color: "aquamarine"
             Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
             visible:list.currentItem
         }
    }



}
