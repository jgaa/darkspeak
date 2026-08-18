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
    // Drafts intentionally live only for the lifetime of this page/application.
    property var messageDrafts: ({})
    property bool restoringDraft: false

    function draftKey(conversation) {
        if (!conversation || !identities.current)
            return ""
        return identities.current.id + ":" + conversation.id
    }

    function saveDraft() {
        var key = draftKey(conversations.current)
        if (key.length > 0)
            messageDrafts[key] = textinput.text
    }

    function restoreDraft() {
        restoringDraft = true
        var key = draftKey(conversations.current)
        textinput.text = key.length > 0 && messageDrafts[key] !== undefined
                ? messageDrafts[key] : ""
        restoringDraft = false
    }

    function scrollActiveConversationToEnd() {
        if (conversations.current && messageList.count > 0) {
            messageList.positionViewAtEnd()
            messageList.currentIndex = messageList.count - 1
        }
    }

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
            Qt.callLater(root.restoreDraft)
        }

        function onCurrentRowChanged() {
            list.currentIndex = conversations.currentRow
            messages.setConversation(conversations.current)
            Qt.callLater(root.restoreDraft)
            Qt.callLater(root.clearActiveConversationNotifications)
            Qt.callLater(root.scrollActiveConversationToEnd)
        }
    }

    Connections {
        target: identities
        function onCurrentIdentityChanged() {
            Qt.callLater(root.restoreDraft)
        }
    }

    Connections {
        target: messages
        function onModelReset() {
            Qt.callLater(root.scrollActiveConversationToEnd)
        }
    }

    Connections {
        target: manager
        function onCurrentPageChanged() {
            Qt.callLater(root.clearActiveConversationNotifications)
        }
    }

    Connections {
        target: mainWindow
        function onActiveChanged() {
            if (mainWindow.active)
                Qt.callLater(root.clearActiveConversationNotifications)
        }
    }

    function clearActiveConversationNotifications() {
        if (manager.currentPage === Manager.CONVERSATIONS && root.visible
                && conversations.current) {
            notifications.clearConversationNotifications(conversations.current.id)
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
                                text: cco ? cco.contactName : ""
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
                        onClicked: {
                            list.currentIndex = index
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
                      ? qsTr("Chat with ")
                        + conversations.current.contactName
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
                            root.saveDraft()
                        }

                        onTextChanged: {
                            if (!root.restoringDraft)
                                root.saveDraft()
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
