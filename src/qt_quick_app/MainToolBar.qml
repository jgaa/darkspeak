import QtQuick 2.9
import QtQuick.Controls 2.4
import QtQuick.Layouts 1.12
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

ToolBar {
    id: root

    Flow {
        anchors.fill: parent
        spacing: 6

        ToolButton {
            text: (manager.onlineState === 0 || manager.onlineState === 4) ? qsTr("Connect") : qsTr("Disconnect")
            icon.source: manager.onlineStatusIcon
            height: parent.height

            icon.color: {
                switch(manager.onlineState) {
                case 0: return "white"
                case 1: return "skyblue"
                case 2: return "yellow"
                case 3: return "lightgreen"
                case 4: return "pink"
                }
            }

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
                    "clip"     : clip,
                    "identity" : identities.getCurrentIdentity()
                });
                dlg.open()
            }
        }

        ToolButton {
            text: qsTr("Send File")
            icon.source: "qrc:/images/FileUpload.svg"
            height: parent.height
            visible: ((manager.currentPage === Manager.CHAT) || (manager.currentPage === Manager.CONVERSATIONS))
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
