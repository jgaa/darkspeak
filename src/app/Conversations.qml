import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Page {
    id: root
    property bool hasConversation: !!conversations.current
    property bool narrow: width < 760 * appearance.factor
    property bool showMessages: false
    property bool restoringDraft: false
    property bool followLatest: true
    property var activeConversation: null

    function restoreDraft() {
        restoringDraft = true;
        composer.text = messages.draft;
        restoringDraft = false;
    }
    function clearNotifications() {
        if (manager.currentPage === Manager.CONVERSATIONS && visible && mainWindow.active && hasConversation && (!narrow || showMessages))
            notifications.clearConversationNotifications(conversations.current.id);
    }
    Component.onCompleted: {
        activeConversation = conversations.current;
        restoreDraft();
        showMessages = hasConversation;
        Qt.callLater(history.positionViewAtEnd);
    }
    Connections {
        target: conversations
        function onCurrentRowChanged() {
            if (root.activeConversation !== conversations.current) {
                root.activeConversation = conversations.current;
                root.restoreDraft();
                root.followLatest = true;
                root.showMessages = root.hasConversation;
            }
            root.clearNotifications();
        }
    }
    Connections {
        target: messages
        function onPageLoaded(direction) {
            if (direction === 0)
                history.positionViewAtEnd();
        }
        function onDraftChanged() {
            if (composer.text !== messages.draft)
                root.restoreDraft();
        }
    }
    Connections {
        target: mainWindow
        function onActiveChanged() {
            root.clearNotifications();
        }
    }
    onVisibleChanged: clearNotifications()
    header: Header {
        whom: identities.current
        text: qsTr("Conversations for ")
    }
    SplitView {
        id: split
        anchors.fill: parent
        onResizingChanged: {
            if (!resizing && !root.narrow) {
                appearance.conversationWidth = conversationPane.width / appearance.factor;
                conversationPane.SplitView.preferredWidth = Qt.binding(function () {
                    return appearance.conversationWidth * appearance.factor;
                });
            }
        }
        handle: Rectangle {
            implicitWidth: 5 * appearance.factor
            color: SplitHandle.hovered || SplitHandle.pressed ? appearance.accent : appearance.border
        }
        Pane {
            id: conversationPane
            visible: !root.narrow || !root.showMessages
            SplitView.preferredWidth: appearance.conversationWidth * appearance.factor
            SplitView.minimumWidth: 180 * appearance.factor
            SplitView.fillWidth: root.narrow
            SplitView.fillHeight: true
            padding: 0

            ListView {
                id: conversationList
                anchors.fill: parent
                model: conversations
                clip: true
                currentIndex: conversations.currentRow
                ScrollBar.vertical: ScrollBar {}
                delegate: ItemDelegate {
                    id: entry
                    required property int index
                    required property var conversation
                    width: conversationList.width
                    height: 68 * appearance.factor
                    highlighted: index === conversations.currentRow
                    onClicked: {
                        conversations.currentRow = index;
                        root.showMessages = true;
                        root.clearNotifications();
                    }
                    contentItem: RowLayout {
                        spacing: 10 * appearance.factor
                        RoundedImage {
                            source: entry.conversation && entry.conversation.participant ? entry.conversation.participant.avatarUrl : ""
                            Layout.preferredWidth: 38 * appearance.factor
                            Layout.preferredHeight: 38 * appearance.factor
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3 * appearance.factor
                            Label {
                                Layout.fillWidth: true
                                text: entry.conversation ? entry.conversation.contactName : ""
                                textFormat: Text.PlainText
                                color: entry.highlighted ? appearance.selectedText : appearance.text
                                font.bold: entry.conversation && entry.conversation.unread > 0
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: entry.conversation ? entry.conversation.topic : ""
                                textFormat: Text.PlainText
                                color: entry.highlighted ? appearance.selectedText : appearance.secondaryText
                                elide: Text.ElideRight
                            }
                        }
                        Label {
                            visible: entry.conversation && entry.conversation.unread > 0
                            text: entry.conversation ? entry.conversation.unread : ""
                            color: entry.highlighted ? appearance.selectedText : appearance.text
                            Accessible.name: qsTr("%1 unread messages").arg(text)
                        }
                    }
                }
            }
            EmptyState {
                anchors.centerIn: parent
                width: parent.width - 24 * appearance.factor
                visible: conversationList.count === 0
                title: qsTr("No conversations yet")
                description: qsTr("Select a contact in People to start a conversation.")
            }
        }
        ColumnLayout {
            visible: !root.narrow || root.showMessages
            SplitView.fillWidth: true
            spacing: 0
            ToolBar {
                Layout.fillWidth: true
                RowLayout {
                    anchors.fill: parent
                    ToolButton {
                        visible: root.narrow
                        text: qsTr("Back")
                        onClicked: root.showMessages = false
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.hasConversation ? conversations.current.contactName : qsTr("Select a conversation")
                        textFormat: Text.PlainText
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Label {
                        visible: root.hasConversation
                        text: root.hasConversation && conversations.current.participant && conversations.current.participant.online ? qsTr("Contact connected") : qsTr("Contact offline")
                        color: appearance.secondaryText
                    }
                    ToolButton {
                        text: qsTr("Details")
                        enabled: root.hasConversation
                        onClicked: details.open()
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                ToolButton {
                    text: qsTr("Older messages")
                    visible: messages.hasOlder
                    enabled: !messages.loading
                    onClicked: {
                        root.followLatest = false;
                        messages.loadOlder();
                    }
                }
                Label {
                    text: messages.loading ? qsTr("Loading history…") : messages.historyError
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    color: messages.historyError.length ? appearance.error : appearance.secondaryText
                }
                ToolButton {
                    text: qsTr("Latest")
                    enabled: root.hasConversation && !messages.loading
                    onClicked: {
                        root.followLatest = true;
                        messages.loadLatest();
                    }
                }
            }
            ListView {
                id: history
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: messages
                clip: true
                spacing: appearance.smallSpacing
                // Visible delegates plus a small nearby buffer; never instantiate the history.
                cacheBuffer: Math.max(0, height)
                ScrollBar.vertical: ScrollBar {}
                onContentYChanged: {
                    if (!messages.loading)
                        root.followLatest = !moving && atYEnd && !messages.hasNewer;
                }
                onMovementStarted: root.followLatest = false
                onMovementEnded: {
                    root.followLatest = atYEnd && !messages.hasNewer;
                    if (root.followLatest)
                        root.clearNotifications();
                    if (atYEnd && messages.hasNewer)
                        messages.loadNewer();
                    else if (atYBeginning && messages.hasOlder)
                        messages.loadOlder();
                }
                Connections {
                    target: messages
                    function onRowsInserted(parent, first, last) {
                        if (root.followLatest && !messages.loading) {
                            Qt.callLater(history.positionViewAtEnd);
                            root.clearNotifications();
                        }
                    }
                }
                delegate: Loader {
                    width: history.width
                    property File cFile: model.file || null
                    property var cData: model
                    property int msgBorder: 16 * appearance.factor
                    property int margin: 4 * appearance.factor
                    property var cList: history
                    property var scrollBar: ({
                            width: 0
                        })
                    source: model.type === MessagesModel.MESSAGE ? "MessageComponent.qml" : "FileComponent.qml"
                }
                DropArea {
                    anchors.fill: parent
                    enabled: root.hasConversation && conversations.current.participant && conversations.current.participant.canSendFiles
                    onDropped: function (drop) {
                        if (drop.hasUrls && drop.urls.length <= 16) {
                            for (let i = 0; i < drop.urls.length; ++i)
                                conversations.current.sendFile({
                                    path: drop.urls[i]
                                });
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: messages.hasNewer
                Item {
                    Layout.fillWidth: true
                }
                Button {
                    text: qsTr("Newer messages")
                    enabled: !messages.loading
                    onClicked: messages.loadNewer()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: appearance.spacing
                spacing: appearance.spacing
                ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 90 * appearance.factor
                    TextArea {
                        id: composer
                        enabled: root.hasConversation
                        placeholderText: qsTr("Write a message… (Ctrl+Enter to send)")
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        Accessible.name: qsTr("Message")
                        onTextChanged: {
                            if (!root.restoringDraft)
                                messages.draft = text;
                        }
                        function send() {
                            if (!root.hasConversation || !text.trim().length)
                                return;
                            if (!conversations.current.sendMessage(text))
                                return;
                            text = "";
                            root.followLatest = true;
                        }
                        Keys.onPressed: function (event) {
                            if ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_Enter || event.key === Qt.Key_Return)) {
                                event.accepted = true;
                                send();
                            }
                        }
                    }
                }
                Button {
                    text: qsTr("Send")
                    enabled: root.hasConversation && composer.text.trim().length > 0
                    onClicked: composer.send()
                }
            }
        }
    }
    UiDialog {
        id: details
        title: qsTr("Connection and identity")
        width: Math.min(root.width - 24 * appearance.factor, 560 * appearance.factor)
        standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: root.hasConversation && conversations.current.participant && conversations.current.participant.peerVerified ? qsTr("Contact key authenticated by the connection protocol.") : qsTr("Contact key has not been authenticated.")
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Manual fingerprint verification is not available yet. Tor connectivity and message delivery do not establish manual verification.")
                wrapMode: Text.WordWrap
            }
            TextArea {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                text: root.hasConversation && conversations.current.participant ? conversations.current.participant.handle : ""
                wrapMode: TextEdit.WrapAnywhere
            }
        }
    }
}
