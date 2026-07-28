import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    id: root
    property bool hasConversation: !!conversations.current
    property color primaryText: "#243447"
    property color secondaryText: "#475569"
    property color selectedFill: "#294d70"
    property color selectedBorder: "#79c2f2"

    header: Header {
        whom: identities.current
        text: qsTr("Conversations for ")
    }

    Connections {
        target: conversations

        function onModelReset() {
            if (conversations.rowCount() === 1)
                list.currentIndex = 0
            else
                list.currentIndex = -1
        }

        function onCurrentRowChanged() {
            list.currentIndex = conversations.currentRow
            messages.setConversation(conversations.current)
        }
    }

    SplitView {
        anchors.fill: parent
        spacing: 0

        handle: Rectangle {
            implicitWidth: 12
            color: SplitHandle.hovered || SplitHandle.pressed ? palette.highlight : palette.mid

            Rectangle {
                anchors.centerIn: parent
                width: 2
                height: parent.height
                color: palette.dark
            }
        }

        Pane {
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: 220
            SplitView.fillHeight: true
            padding: 0

            ListView {
                id: list
                anchors.fill: parent
                interactive: true
                model: conversations
                highlight: highlightBar
                clip: true

                onCurrentIndexChanged: conversations.currentRow = currentIndex

                delegate: Item {
                    id: itemDelegate
                    width: list.width
                    height: 60
                    property Conversation cco: conversation

                    Row {
                        anchors.fill: parent
                        spacing: 4

                        Column {
                            padding: 5
                            anchors.verticalCenter: parent.verticalCenter

                            RoundedImage {
                                source: cco && cco.participant ? cco.participant.avatarUrl : ""
                                height: 45
                                width: 45
                                border.width: 6
                                border.color: cco && cco.participant
                                              ? cco.participant.online ? "lime" : "red"
                                              : "grey"
                            }
                        }

                        Column {
                            spacing: 10
                            anchors.verticalCenter: parent.verticalCenter

                            Text {
                                font.pointSize: 10
                                color: itemDelegate.ListView.isCurrentItem ? "#ffffff" : root.primaryText
                                text: cco && cco.name
                                      ? cco.name
                                      : cco && cco.participant
                                        ? cco.participant.name
                                          ? cco.participant.name
                                          : cco.participant.nickName
                                        : ""
                                font.bold: itemDelegate.ListView.isCurrentItem
                            }

                            Text {
                                font.pointSize: 14
                                color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                                text: cco ? cco.topic : ""
                                font.bold: itemDelegate.ListView.isCurrentItem
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton

                        onClicked: function(mouse) {
                            list.currentIndex = index
                            if (mouse.button === Qt.RightButton) {
                                contextMenu.x = mouse.x
                                contextMenu.y = mouse.y
                                contextMenu.open()
                            }
                        }

                        onPressAndHold: function(mouse) {
                            list.currentIndex = index
                            contextMenu.x = mouse.x
                            contextMenu.y = mouse.y
                            contextMenu.open()
                        }
                    }
                }
            }
        }

        ColumnLayout {
            SplitView.fillWidth: true
            SplitView.fillHeight: true
            spacing: 0

            Label {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                leftPadding: 12
                verticalAlignment: Text.AlignVCenter
                text: root.hasConversation && conversations.current.participant
                      ? qsTr("Chat with ") + conversations.current.participant.name
                      : qsTr("Select a conversation")
                color: root.primaryText
                background: Rectangle { color: "#263746" }
            }

            Rectangle {
                id: chatArea
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#d7d7d7"

                ListView {
                    id: messageList
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: input.top
                    anchors.margins: 4
                    anchors.bottomMargin: 8
                    spacing: 4
                    clip: true
                    model: messages
                    ScrollBar.vertical: ScrollBar { id: scrollbar }

                    function scrollToEnd() {
                        positionViewAtEnd()
                        currentIndex = count - 1
                    }

                    onCountChanged: {
                        if (currentIndex === -1 || currentIndex === count - 2)
                            scrollToEnd()
                    }

                    DropArea {
                        anchors.fill: parent
                        enabled: root.hasConversation
                        onDropped: function(drop) {
                            if (drop.hasUrls) {
                                for (var i = 0; i < drop.urls.length; ++i)
                                    conversations.current.sendFile({"path": drop.urls[i]})
                            }
                        }
                    }

                    delegate: Loader {
                        property File cFile: file ? file : null
                        property var cData: model
                        property int msgBorder: 16
                        property int margin: 4
                        property var cList: messageList
                        property var scrollBar: scrollbar
                        source: type === MessagesModel.MESSAGE
                                ? "MessageComponent.qml" : "FileComponent.qml"
                    }
                }

                Rectangle {
                    id: input
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 4
                    height: 96
                    radius: 4
                    color: "white"
                    border.color: "steelblue"
                    border.width: 1
                    enabled: root.hasConversation

                    TextArea {
                        id: textinput
                        anchors.fill: parent
                        color: "black"
                        clip: true
                        focus: root.hasConversation

                        Keys.onPressed: function(event) {
                            if (event.modifiers && Qt.ControlModifier
                                    && (event.key === Qt.Key_Enter
                                        || event.key === Qt.Key_Return)) {
                                event.accepted = true
                                send()
                            }
                        }

                        function send() {
                            if (!root.hasConversation)
                                return
                            conversations.current.sendMessage(text)
                            text = ""
                        }
                    }
                }

                Button {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 4
                    text: qsTr("Send")
                    icon.source: "qrc:/images/Sent-32.png"
                    enabled: root.hasConversation
                    onClicked: textinput.send()
                }
            }
        }
    }

    Menu {
        id: contextMenu

        MenuItem {
            text: qsTr("Open")
            onTriggered: list.currentIndex = conversations.currentRow
        }
    }

    Component {
        id: highlightBar
        Rectangle {
            radius: 5
            opacity: 1
            y: list.currentItem ? list.currentItem.y : 0
            color: root.selectedFill
            border.color: root.selectedBorder
            visible: list.currentItem
            Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
        }
    }
}
