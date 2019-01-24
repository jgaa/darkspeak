import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.3
import com.jgaa.darkspeak 1.0

Page {
    width: 600
    height: 400
    clip: false

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
        property bool isOnline : model.getOnlineStatus(currentIndex)

        delegate: Item {
            id: itemDelegate
            width: parent.width
            height: 112

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
                        border.color: online ? "lime" : "firebrick"
                        color: "black"

                        Image {
                            id: avatar
                            fillMode: Image.PreserveAspectFit
                            height: 96
                            width: 96
                            x: 2
                            y: 2
                            source: "qrc:///images/anonymous.svg"

                            Rectangle {
                                height: parent.width / 3
                                color: online ? "lime" : "firebrick"
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

                Column {
                    spacing: 10
                    x: 116
                    Text {
                        font.pointSize: 14
                        color: "white"
                        text: name
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }

                    GridLayout {
                        rowSpacing: 0
                        rows: 3
                        flow: GridLayout.TopToBottom

                        Label { font.pointSize: 9; text: qsTr("Created")}
                        Label { font.pointSize: 9; text: qsTr("handle")}
                        Label { font.pointSize: 9; text: qsTr("Onion")}

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

        function deleteCurrent() {
            identities.deleteIdentity(currentIndex);
        }

        function toggleOnline() {
            if (isOnline) {
                model.stopService(currentIndex);
            } else {
                model.startService(currentIndex);
            }
        }

        function copyHandle() {
            manager.textToClipboard(identities.get(currentIndex).handle)
        }

        function copyOnion() {
            manager.textToClipboard(identities.get(currentIndex).onion)
        }

        function copyIdentity() {
            manager.textToClipboard(
                        identities.get(currentIndex).name + ':' +
                        identities.getIdentityAsBase58(currentIndex))
        }

        function createNewTransport() {
            identities.createNewTransport(currentIndex)
        }
    }

    Component {
         id: highlightBar
         Rectangle {
             radius: 5
             y: list.currentItem.y;
             anchors.fill: list.currentItem.width
             color: "#19462a"
             border.color: "lime"
             //Behavior on y { SpringAnimation { spring: 2; damping: 0.1 } }
         }
     }

    Menu {
        id: contextMenu

        onAboutToShow: prepare()

        function prepare() {
            online.text =  list.isOnline ? qsTr("Stop Tor service") : qsTr("Start Tor service")
        }

        MenuItem {
            id: online
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
            text: qsTr("Copy handle")
            icon.name: "edit-copy"
            onTriggered: list.copyHandle()
        }

        MenuItem {
            text: qsTr("Copy onion")
            icon.name: "edit-copy"
            onTriggered: list.copyOnion()
        }

        MenuItem {
            text: qsTr("Change nickname")
            icon.source: "qrc:///images/user.svg"
        }

        MenuItem {
            text: qsTr("Select picture")
            icon.name: "document-open"
        }

        MenuItem {
            text: qsTr("Export identity")
            icon.name: "document-save"
        }

        MenuItem {
            text: qsTr("New Tor service")
            icon.name: "document-new"
            onTriggered: confirmNewTransport.open()
            enabled: manager.online
        }

        MenuItem {
            text: qsTr("Export Tor service")
            icon.name: "document-save"
        }

        MenuItem {
            text: qsTr("Import Tor service")
            icon.name: "document-open"
        }

        MenuItem {
            text: qsTr("Delete")
            icon.name: "edit-delete"
            onTriggered: confirmDelete.open()
        }
    }

    MessageDialog {
        id: confirmDelete
        icon: StandardIcon.Warning
        title: qsTr("Delete Identity")
        text: qsTr("Do you really want to delete this identity?")
        standardButtons: MessageDialog.Yes | MessageDialog.Cancel
        detailedText: qsTr("If you delete this identity, all its related contacts and messages will also be deleted.")
        onYes: list.deleteCurrent()
    }

    MessageDialog {
        id: confirmNewTransport
        icon: StandardIcon.Warning
        title: qsTr("New Transport")
        text: qsTr("Do you really want to change the Transport address?")
        standardButtons: MessageDialog.Yes | MessageDialog.Cancel
        detailedText: qsTr("If you change your transport address, none of your current contacts or anyone else will be able to contact you on your current address.")
        onYes: list.createNewTransport();
    }
}
