import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

ToolBar {
    id: root

    implicitHeight: appearance.toolbarHeight

    RowLayout {
        anchors.fill: parent
        spacing: 6 * appearance.factor

        ToolButton {
            text: (manager.onlineState === 0 || manager.onlineState === 4) ? qsTr("Connect") : qsTr("Disconnect")
            Layout.fillHeight: true

            onClicked: {
                if (manager.onlineState === 0 || manager.onlineState === 4) {
                    manager.goOnline();
                } else {
                    manager.goOffline();
                }
            }
        }

        ToolButton {
            text: qsTr("Identity")
            icon.name: "document-new"
            Layout.fillHeight: true
            visible: manager.currentPage === Manager.IDENTITIES
            enabled: manager.online

            onClicked: {
                manager.clearTmpImage();
                var component = Qt.createComponent("qrc:/EditIdentityDialog.qml");
                if (component.status !== Component.Ready) {
                    if (component.status === Component.Error)
                        console.debug("Error:" + component.errorString());
                    return;
                }
                var dlg = component.createObject(mainWindow, {});
                dlg.open();
            }
        }

        ToolButton {
            text: qsTr("Paste new contact")
            icon.name: "content-paste"
            Layout.fillHeight: true
            visible: manager.currentPage === Manager.CONTACTS
            enabled: manager.hasValidContactInClipboard

            onClicked: {
                manager.clearTmpImage();
                var clip = manager.getIdenityFromClipboard();
                if (!clip || !clip.identity)
                    return;
                var component = Qt.createComponent("qrc:/EditContactDialog.qml");
                if (component.status !== Component.Ready) {
                    if (component.status === Component.Error)
                        console.debug("Error:" + component.errorString());
                    return;
                }
                var dlg = component.createObject(mainWindow, {
                    "clipboardData": clip,
                    "identity": identities.getCurrentIdentity()
                });
                dlg.open();
            }
        }

        ToolButton {
            text: qsTr("Send File")
            icon.source: appearance.asset("sendFile")
            icon.color: enabled ? appearance.text : appearance.disabled
            Layout.fillHeight: true
            visible: manager.currentPage === Manager.CONVERSATIONS && conversations.current && conversations.current.participant && conversations.current.participant.canSendFiles

            onClicked: {
                var component = Qt.createComponent("qrc:/EditUploadFileDialog.qml");
                if (component.status !== Component.Ready) {
                    if (component.status === Component.Error)
                        console.debug("Error:" + component.errorString());
                    return;
                }
                var dlg = component.createObject(mainWindow, {
                    "parent": mainWindow,
                    "conversation": conversations.current
                });
                dlg.open();
            }
        }

        ToolButton {
            text: qsTr("Settings")
            icon.name: "document-properties"
            Layout.fillHeight: true
            visible: (manager.currentPage === Manager.HOME)

            onClicked: mainWindow.openSettings()
        }
        Item {
            Layout.fillWidth: true
        }
        ScaleControl {}
    }
}
