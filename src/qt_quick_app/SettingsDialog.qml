import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.2

Dialog {
    id: root
    standardButtons: StandardButton.Save | StandardButton.Cancel
    title: qsTr("Settings")
    property int dlg_width: mainWindow.width > 400 ? 400 : mainWindow.width
    property int dlg_height: mainWindow.height > 500 ? 370 : mainWindow.height
    width: dlg_width
    height: dlg_height

    TabView {
        id: ownInfoView
        anchors.fill: parent

        Tab {
            anchors.rightMargin: 6
            anchors.leftMargin: 6
            anchors.bottomMargin: 6
            anchors.topMargin: 6
            anchors.fill: parent
            title: qsTr("Tor")
            TorSettings { }
        }

        Tab {
            anchors.rightMargin: 6
            anchors.leftMargin: 6
            anchors.bottomMargin: 6
            anchors.topMargin: 6
            anchors.fill: parent
            title: qsTr("Paths")
            PathsSettings { }
        }

        Tab {
            anchors.rightMargin: 6
            anchors.leftMargin: 6
            anchors.bottomMargin: 6
            anchors.topMargin: 6
            anchors.fill: parent
            title: qsTr("Logging")
            LogSettings { }
        }
    }

    onAccepted: {
        for(var i = 0; i < ownInfoView.count; i++) {
            // We can only commit from pages that has been displayed it appears...
            var item =  ownInfoView.getTab(i).item
            if (item !== null) {
                item.commit()
            }
        }
        close()
    }

    onRejected: {
        close()
    }
}

