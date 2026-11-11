import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    id: root
    clip: true

    property color primaryText: appearance.text
    property color secondaryText: appearance.secondaryText
    property color labelText: appearance.secondaryText
    property color selectedFill: appearance.selected
    property color selectedBorder: appearance.accent

    header: Label {
        textFormat: Text.PlainText
        text: qsTr("Your Identities")
        font.pixelSize: appearance.fontPixels * 2
        padding: 10 * appearance.factor
    }

    ListView {
        id: list
        interactive: true
        model: identities
        anchors.fill: parent
        highlight: highlightBar
        property Identity currentIdentity: identities.current
        property bool online: currentIdentity ? currentIdentity.online : false
        // The identity manager starts with no selected row. Select the first
        // identity so contacts and conversations are initialized on startup.
        currentIndex: identities.currentRow >= 0 ? identities.currentRow : (count > 0 ? 0 : -1)

        onCurrentIndexChanged: {
            if (currentIndex !== identities.currentRow)
                identities.setCurrentIdentity(currentIndex);
        }

        delegate: ItemDelegate {
            id: itemDelegate
            required property int index
            required property var identity
            property var cid: identity
            width: list.width
            height: card.implicitHeight + 24 * appearance.factor
            highlighted: ListView.isCurrentItem
            onClicked: list.currentIndex = index
            Accessible.name: cid ? cid.name : ""
            contentItem: ColumnLayout {
                id: card
                spacing: 6 * appearance.factor
                RowLayout {
                    Layout.fillWidth: true
                    RoundedImage {
                        source: cid ? cid.avatarUrl : ""
                        Layout.preferredWidth: 42 * appearance.factor
                        Layout.preferredHeight: 42 * appearance.factor
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: cid ? cid.name : ""
                            textFormat: Text.PlainText
                            font.bold: true
                            color: itemDelegate.highlighted ? appearance.selectedText : appearance.text
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: cid && cid.online ? qsTr("Onion service running") : qsTr("Onion service stopped")
                            textFormat: Text.PlainText
                            color: itemDelegate.highlighted ? appearance.selectedText : appearance.secondaryText
                            wrapMode: Text.WordWrap
                        }
                    }
                    ToolButton {
                        text: qsTr("Actions…")
                        Accessible.name: qsTr("Actions for %1").arg(cid ? cid.name : "")
                        onClicked: {
                            list.currentIndex = index;
                            contextMenu.popup();
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: cid ? Qt.formatDateTime(cid.created, Qt.DefaultLocaleShortDate) : ""
                    textFormat: Text.PlainText
                    color: itemDelegate.highlighted ? appearance.selectedText : appearance.secondaryText
                    wrapMode: Text.WordWrap
                }
                TextEdit {
                    Layout.fillWidth: true
                    text: cid ? cid.handle : ""
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
                    text: cid ? cid.address : ""
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

        function deleteCurrent() {
            identities.removeIdentity(currentIdentity.uuid);
        }

        function toggleOnline() {
            if (currentIdentity.online) {
                currentIdentity.stopService();
            } else {
                currentIdentity.startService();
            }
        }

        function copyHandle() {
            manager.textToClipboard(currentIdentity.handle);
        }

        function copyIdentity() {
            manager.textToClipboard(manager.getIdentityText(currentIdentity.name, currentIdentity.b58identity));
        }

        function createIdentityQrCode() {
            manager.createIdentityQrCode(currentIdentity.name, currentIdentity.b58identity);
        }

        function createNewTransport() {
            currentIdentity.changeTransport();
        }

        function editCurrent() {
            var component = Qt.createComponent("qrc:/EditIdentityDialog.qml");
            var dlg = component.createObject(mainWindow, {
                "parent": mainWindow,
                "identity": currentIdentity
            });
            dlg.open();
        }

        function importTorService() {
            var component = Qt.createComponent("qrc:/ImportTorServiceDialog.qml");
            var dlg = component.createObject(mainWindow, {
                "parent": mainWindow,
                "identity": currentIdentity
            });
            dlg.open();
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
        }
    }

    Menu {
        id: contextMenu

        onAboutToShow: prepare()

        function prepare() {
        //online.text = list.online ? qsTr("Stop Tor service") : qsTr("Start Tor service")
        }

        MenuItem {
            id: online
            text: list.online ? qsTr("Stop Tor service") : qsTr("Start Tor service")
            icon.source: "qrc:///images/onion-bw.svg"
            onTriggered: list.toggleOnline()
            enabled: manager.online
        }

        MenuItem {
            text: qsTr("Copy identity")
            icon.name: "edit-copy"
            onTriggered: list.copyIdentity()
        }

        MenuItem {
            text: qsTr("Create QR Code")
            icon.source: "qrc:///images/qr-code.svg"
            onTriggered: list.createIdentityQrCode()
        }

        // MenuItem {
        //     text: qsTr("Copy handle")
        //     icon.name: "edit-copy"
        //     onTriggered: list.copyHandle()
        // }

        MenuItem {
            text: qsTr("Edit")
            icon.name: "insert-text"
            onTriggered: list.editCurrent()
        }

        // MenuItem {
        //     text: qsTr("New Tor service")
        //     icon.name: "document-new"
        //     onTriggered: confirmNewTransport.open()
        //     enabled: manager.online
        // }

        // MenuItem {
        //     text: qsTr("Import Tor service")
        //     icon.name: "document-open"
        //     onTriggered: list.importTorService()
        // }

        MenuItem {
            text: qsTr("Delete")
            icon.name: "edit-delete"
            onTriggered: confirmDelete.open()
        }
    }

    MessageDialog {
        id: confirmDelete
        title: qsTr("Delete Identity")
        text: qsTr("Do you really want to delete this identity?")
        buttons: MessageDialog.Yes | MessageDialog.Cancel
        detailedText: qsTr("If you delete this identity, all its related contacts and messages will also be deleted.")
        onButtonClicked: function (button) {
            if (button === MessageDialog.Yes)
                list.deleteCurrent();
        }
    }

    MessageDialog {
        id: confirmNewTransport
        title: qsTr("New Transport")
        text: qsTr("Do you really want to change the Transport address?")
        buttons: MessageDialog.Yes | MessageDialog.Cancel
        detailedText: qsTr("If you change your transport address, none of your current contacts or anyone else will be able to contact you on your current address.")
        onButtonClicked: function (button) {
            if (button === MessageDialog.Yes)
                list.createNewTransport();
        }
    }
}
