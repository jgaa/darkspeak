import QtQuick 2.9
import QtQuick.Controls 2.4
import QtQuick.Layouts 1.12
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

ToolBar {
    id: mainToolBar

    RowLayout{
        height: parent.height

        ToolButton {
            text: qsTr("Connect")
            icon.source: manager.onlineStatusIcon
            checkable: false
            Layout.preferredHeight: parent.height
            //anchors.top: parent.top

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
            text: qsTr("+Identity")
            checkable: false
            Layout.preferredHeight: parent.height
            visible: manager.currentPage === 1

             onClicked: {
                 var popupComponent = Qt.createComponent("qrc:/CreateIdentityDialog.qml")
                 var dlg = popupComponent.createObject(mainWindow, {"parent" : mainWindow});
                 dlg.open()
             }
        }

    }
}
