import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    id: root
    clip: true

    property color primaryText: "#243447"
    property color secondaryText: "#475569"
    property color labelText: "#64748b"
    property color selectedFill: "#294d70"
    property color selectedBorder: "#79c2f2"

    header: Label {
        text: qsTr("Your Identities")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }


    ListView {
        id: list
        interactive: true
        model: identities
        anchors.fill: parent
        highlight: highlightBar
        property Identity currentIdentity: identities.current
        property bool online: currentIdentity ? currentIdentity.online : false

        onCurrentItemChanged: {
            identities.setCurrentIdentity(currentIndex)

            // This is where we synchronize the Contacts and conversations with the current selected identity
            if (currentIdentity) {
                contacts.setIdentity(currentIdentity.uuid)
                conversations.setIdentity(currentIdentity.uuid)
            }
        }

        delegate: Item {
            id: itemDelegate
            width: list.width
            height: 112
            property Identity cid : identity

            Row {
                id: row1
                anchors.fill: parent
                spacing: 8

                Column {
                    padding: 5
                    anchors.verticalCenter: parent.verticalCenter

                    Rectangle {
                        id: avatarFrame
                        height: 100
                        width: 100
                        radius: 5
                        border.color: cid.online ? "lime" : "firebrick"
                        color: "black"

                        Image {
                            id: avatar
                            fillMode: Image.PreserveAspectFit
                            height: 96
                            width: 96
                            x: 2
                            y: 2
                            source: cid.avatarUrl
                            cache: false

                            Rectangle {
                                height: parent.width / 3
                                color: cid.online ? "lime" : "firebrick"
                                width: height
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.topMargin: 0
                                radius: width*0.5

                                Image {
                                    id: torStatus
                                    anchors.fill: parent
                                    source: "qrc:///images/onion-bw.svg"
                                }
                            }
                        }
                    }
                }

                Column {
                    spacing: 10
                    Text {
                        font.pointSize: 14
                        color: itemDelegate.ListView.isCurrentItem ? "#ffffff" : root.primaryText
                        text: cid.name
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }

                    GridLayout {
                        rowSpacing: 0
                        rows: 3
                        flow: GridLayout.TopToBottom

                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("Created")}
                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("handle")}
                        Label { font.pointSize: 9; color: root.labelText; text: qsTr("Onion")}

                        FormattedTime {
                            font.pointSize: 9;
                            color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                            value: cid.created
                        }

                        Text {
                            font.pointSize: 9;
                            color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                            text: cid.handle
                        }

                        Text {
                            font.pointSize: 9;
                            color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                            text: cid.address
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

        function deleteCurrent() {
            identities.removeIdentity(currentIdentity.uuid);
        }

        function toggleOnline() {
            if (currentIdentity.online) {
                currentIdentity.stopService()
            } else {
                currentIdentity.startService()
            }
        }

        function copyHandle() {
            manager.textToClipboard(currentIdentity.handle)
        }

        function copyIdentity() {
            manager.textToClipboard(manager.getIdentityText(currentIdentity.name,
                                                            currentIdentity.b58identity))
        }

        function createIdentityQrCode() {
            manager.createIdentityQrCode(currentIdentity.name,
                                         currentIdentity.b58identity)
        }

        function createNewTransport() {
            currentIdentity.changeTransport()
        }

        function editCurrent() {
            var component = Qt.createComponent("qrc:/EditIdentityDialog.qml")
            var dlg = component.createObject(mainWindow, {"parent" : mainWindow,
                                              "identity" : currentIdentity});
            dlg.open()
        }

        function importTorService() {
            var component = Qt.createComponent("qrc:/ImportTorServiceDialog.qml")
            var dlg = component.createObject(mainWindow, {"parent" : mainWindow,
                                              "identity" : currentIdentity});
            dlg.open()
        }
    }

    ImportTorServiceDialog {
        id: gakke
    }

    Component {
         id: highlightBar
         Rectangle {
             radius: 5
             opacity: 1
             y: list.currentItem.y;
             color: root.selectedFill
             border.color: root.selectedBorder
             Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
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
            onTriggered: list.copyIdentity();
        }

        MenuItem {
            text: qsTr("Create QR Code")
            icon.source: "qrc:///images/qr-code.svg"
            onTriggered: list.createIdentityQrCode()
        }

        MenuItem {
            text: qsTr("Copy handle")
            icon.name: "edit-copy"
            onTriggered: list.copyHandle()
        }

        MenuItem {
            text: qsTr("Edit")
            icon.name: "insert-text"
            onTriggered: list.editCurrent()
        }

        MenuItem {
            text: qsTr("New Tor service")
            icon.name: "document-new"
            onTriggered: confirmNewTransport.open()
            enabled: manager.online
        }

        MenuItem {
            text: qsTr("Import Tor service")
            icon.name: "document-open"
            onTriggered: list.importTorService()
        }

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
        onButtonClicked: function(button) {
            if (button === MessageDialog.Yes) list.deleteCurrent()
        }
    }

    MessageDialog {
        id: confirmNewTransport
        title: qsTr("New Transport")
        text: qsTr("Do you really want to change the Transport address?")
        buttons: MessageDialog.Yes | MessageDialog.Cancel
        detailedText: qsTr("If you change your transport address, none of your current contacts or anyone else will be able to contact you on your current address.")
        onButtonClicked: function(button) {
            if (button === MessageDialog.Yes) list.createNewTransport()
        }
    }
}
