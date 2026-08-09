import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

ToolBar {
    id: root

    Flow {
        anchors.fill: parent
        spacing: 6

        ToolButton {
            text: (manager.onlineState === 0 || manager.onlineState === 4) ? qsTr("Connect") : qsTr("Disconnect")
            height: parent.height

            onClicked: {
                console.log("Online status is " + manager.onlineState)
                if (manager.onlineState === 0 || manager.onlineState === 4) {
                    manager.goOnline()
                } else {
                    manager.goOffline()
                }
            }
        }

        ToolButton {
            text: qsTr("Identity")
            icon.name: "document-new"
            height: parent.height
            visible: manager.currentPage === Manager.IDENTITIES
            enabled: manager.online

            onClicked: {
                manager.clearTmpImage();
                var component = Qt.createComponent("qrc:/EditIdentityDialog.qml")
                if (component.status !== Component.Ready) {
                    if(component.status === Component.Error )
                        console.debug("Error:"+ component.errorString() );
                    return;
                }
                var dlg = component.createObject(mainWindow, {"parent" : mainWindow});
                dlg.open()
            }
        }

        ToolButton {
            text: qsTr("Contact")
            icon.name: "document-new"
            height: parent.height
            visible: manager.currentPage === Manager.CONTACTS

            onClicked: {
                manager.clearTmpImage();
                var clip = manager.getIdenityFromClipboard()
                var component = Qt.createComponent("qrc:/EditContactDialog.qml")
                if (component.status !== Component.Ready) {
                    if(component.status === Component.Error )
                        console.debug("Error:"+ component.errorString() );
                    return;
                }
                var dlg = component.createObject(mainWindow, {
                    "parent"   : mainWindow,
                    "clipboardData" : clip,
                    "identity" : identities.getCurrentIdentity()
                });
                dlg.open()
            }
        }

        ToolButton {
            text: qsTr("Send File")
            icon.source: "qrc:/images/FileUpload.svg"
            height: parent.height
            visible: manager.currentPage === Manager.CONVERSATIONS
                     && conversations.current

            onClicked: {
                var component = Qt.createComponent("qrc:/EditUploadFileDialog.qml")
                if (component.status !== Component.Ready) {
                    if(component.status === Component.Error )
                        console.debug("Error:"+ component.errorString() );
                    return;
                }
                var dlg = component.createObject(mainWindow, {
                    "parent"   : mainWindow,
                    "conversation" : conversations.current
                });
                dlg.open()
            }
        }

        ToolButton {
            text: qsTr("Settings")
            icon.name: "document-properties"
            height: parent.height
            visible: (manager.currentPage === Manager.HOME)

            onClicked: {
                var component = Qt.createComponent("qrc:/SettingsDialog.qml")
                if (component.status !== Component.Ready) {
                    if(component.status === Component.Error )
                        console.debug("Error:"+ component.errorString() );
                    return;
                }
                var dlg = component.createObject(mainWindow, {
                    "parent"   : mainWindow,
                    "conversation" : conversations.current
                });
                dlg.open()
            }
        }
    }
}
