import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    id: root

    property color primaryText: appearance.text
    property color secondaryText: appearance.secondaryText
    property color labelText: appearance.secondaryText
    property color selectedFill: appearance.selected
    property color selectedBorder: appearance.accent

    header: ColumnLayout {
        Header {
            Layout.fillWidth: true
            whom: identities.current
            text: qsTr("People for ")
        }
        TextField {
            Layout.fillWidth: true
            Layout.margins: appearance.spacing
            placeholderText: qsTr("Search people by name")
            Accessible.name: qsTr("Search people")
            onTextEdited: contacts.search = text
        }
    }

    property var states: [qsTr("Pending"), qsTr("Waiting"), qsTr("OK"), qsTr("Rejected"), qsTr("BLOCKED")]
    Connections {
        target: contacts
        function onModelReset() {
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

        delegate: ItemDelegate {
            id: itemDelegate
            required property int index
            required property var contact
            property var cco: contact
            width: list.width
            height: card.implicitHeight + 24 * appearance.factor
            highlighted: ListView.isCurrentItem
            onClicked: {
                list.currentIndex = index;
                conversations.setContact(cco);
            }
            Accessible.name: cco ? (cco.name || cco.nickName) : ""
            contentItem: ColumnLayout {
                id: card
                spacing: 6 * appearance.factor
                RowLayout {
                    Layout.fillWidth: true
                    RoundedImage {
                        source: cco ? cco.avatarUrl : ""
                        Layout.preferredWidth: 42 * appearance.factor
                        Layout.preferredHeight: 42 * appearance.factor
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: cco ? (cco.name || cco.nickName) : ""
                            textFormat: Text.PlainText
                            font.bold: true
                            color: itemDelegate.highlighted ? appearance.selectedText : appearance.text
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: !cco ? "" : cco.iBlocked ? qsTr("Blocked") : cco.online ? qsTr("Contact connected") : qsTr("Contact offline")
                            textFormat: Text.PlainText
                            color: itemDelegate.highlighted ? appearance.selectedText : appearance.secondaryText
                            wrapMode: Text.WordWrap
                        }
                    }
                    ToolButton {
                        text: qsTr("Actions…")
                        Accessible.name: qsTr("Actions for %1").arg(cco ? (cco.name || cco.nickName) : "")
                        onClicked: {
                            list.currentIndex = index;
                            contextMenu.popup();
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: cco && cco.peerVerified ? qsTr("Contact key authenticated") : qsTr("Contact key not authenticated")
                    textFormat: Text.PlainText
                    color: itemDelegate.highlighted ? appearance.selectedText : appearance.secondaryText
                    wrapMode: Text.WordWrap
                }
                TextEdit {
                    Layout.fillWidth: true
                    text: cco ? cco.handle : ""
                    textFormat: TextEdit.PlainText
                    font: appearance.font
                    color: itemDelegate.highlighted ? appearance.selectedText : appearance.text
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    Accessible.name: qsTr("Contact identifier")
                }
                TextEdit {
                    Layout.fillWidth: true
                    text: cco ? cco.address : ""
                    textFormat: TextEdit.PlainText
                    font: appearance.font
                    color: itemDelegate.highlighted ? appearance.selectedText : appearance.secondaryText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    Accessible.name: qsTr("Onion address")
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
            manager.currentPage = Manager.CONVERSATIONS;
        }

        function editCurrent() {
            var component = Qt.createComponent("qrc:/EditContactDialog.qml");
            var dlg = component.createObject(mainWindow, {
                "contact": currentItem.cco
            });
            dlg.open();
        }

        function copy() {
            manager.textToClipboard(manager.getIdentityText(currentItem.cco.nickName, currentItem.cco.b58identity));
        }

        function showBio() {
            var component = Qt.createComponent("qrc:/BioDialog.qml");
            var dlg = component.createObject(mainWindow, {
                "contact": currentItem.cco
            });
            dlg.open();
        }

        function deleteCurrent() {
            if (currentItem && currentItem.cco) {
                manager.deleteContact(currentItem.cco.uuid);
            }
        }
    }

    Component {
        id: highlightBar
        Rectangle {
            radius: 5 * appearance.factor
            opacity: 1
            y: list.currentItem ? list.currentItem.y : 0
            color: root.selectedFill
            border.color: root.selectedBorder
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
            text: qsTr("Bio")
            icon.name: "user-info"
            enabled: list.currentItem && list.currentItem.cco && list.currentItem.cco.hasBio
            onTriggered: list.showBio()
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
        onButtonClicked: function (button) {
            if (button === MessageDialog.Yes) {
                list.deleteCurrent();
            }
        }
    }
}

/*##^##
Designer {
    D{i:0;autoSize:true;height:480 * appearance.factor;width:640 * appearance.factor}
}
##^##*/
