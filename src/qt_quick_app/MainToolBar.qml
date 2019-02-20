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
            text: qsTr("Connect")
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
                    text = qsTr("Disconnect")
                } else {
                    manager.goOffline()
                    text = qsTr("Connect")
                }
            }
        }

        ToolButton {
            text: qsTr("Identity")
            icon.name: "document-new"
            height: parent.height
            visible: manager.currentPage === 1

            onClicked: {
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
            visible: manager.currentPage === 2

            onClicked: {
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
    }
}
