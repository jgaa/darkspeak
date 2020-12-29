import QtQuick 2.12
import QtQuick.Controls 2.5
import Qt.labs.settings 1.0
import com.jgaa.darkspeak 1.0

ApplicationWindow {
    id: mainWindow
    visible: true
    width: 640
    height: 480
    title: manager.programNameAndVersion

    Settings {
        category: "ApplicationWindow"
        property alias x: mainWindow.x
        property alias y: mainWindow.y
        property alias width: mainWindow.width
        property alias height: mainWindow.height
    }

    Settings {
        id: appSettings
        property bool appAutoConnect : true
    }

    Component.onCompleted: {
        if (appSettings.appAutoConnect) {
            manager.goOnline()
        }
    }

    header: MainToolBar {
        id: mainToolBar
        height: 32
        width: parent.width
    }

    SwipeView {
        id: swipeView
        anchors.fill: parent
        currentIndex: manager.currentPage

        onCurrentIndexChanged: {
            manager.currentPage = currentIndex
        }

        Log {
        }

        Identities {
        }

        Contacts {
        }

        Home {
        }

        Conversations {
        }

        Chat {
        }
    }

    PageIndicator {
        id: indicator
        visible: !tabBar.visible

        count: swipeView.count
        currentIndex: swipeView.currentIndex

        anchors.bottom: swipeView.bottom
        anchors.horizontalCenter: parent.horizontalCenter
    }

    footer: TabBar {
        id: tabBar
        currentIndex: manager.currentPage
        visible: width > 600

        onCurrentIndexChanged: {
            manager.currentPage = currentIndex
        }

        TabButton {
            text: qsTr("Log")
        }
        TabButton {
            text: qsTr("Identities")
        }
        TabButton {
            text: qsTr("Contacts")
        }
        TabButton {
            text: qsTr("Home")
        }
        TabButton {
            text: qsTr("Conversations")
        }
        TabButton {
            text: qsTr("Chat")
        }
    }
}
