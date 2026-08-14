import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: root
    standardButtons: Dialog.Save | Dialog.Cancel
    title: qsTr("Settings")
    anchors.centerIn: parent
    width: Math.min(Math.max(mainWindow.width - 80, 480), 900)
    height: Math.min(Math.max(mainWindow.height - 80, 360), 760)
    padding: 16

    contentItem: ColumnLayout {
        spacing: 12

        TabBar {
            id: tabs
            Layout.fillWidth: true

            onCurrentIndexChanged: root.loadPage(currentIndex)

            TabButton { text: qsTr("General") }
            TabButton { text: qsTr("Tor") }
            TabButton { text: qsTr("Paths") }
            TabButton { text: qsTr("Logging") }
            TabButton { text: qsTr("Limits") }
        }

        ScrollView {
            id: pageScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Loader {
                id: pageLoader
                width: pageScroll.availableWidth
                height: Math.max(pageScroll.availableHeight,
                                 item && item.implicitHeight ? item.implicitHeight : 0)
            }
        }
    }

    function loadPage(index) {
        if (pageLoader.item && pageLoader.item.commit) {
            pageLoader.item.commit()
        }
        pageLoader.sourceComponent = pageComponents[index]
    }

    property var pageComponents: [generalPage, torPage, pathsPage, loggingPage, limitsPage]

    Component {
        id: generalPage
        GeneralSettings {}
    }

    Component {
        id: torPage
        TorSettings {}
    }

    Component {
        id: pathsPage
        PathsSettings {}
    }

    Component {
        id: loggingPage
        LogSettings {}
    }

    Component {
        id: limitsPage
        ResourceLimitsSettings {}
    }

    Component.onCompleted: loadPage(tabs.currentIndex)

    onAccepted: {
        if (pageLoader.item && pageLoader.item.commit) {
            pageLoader.item.commit()
        }
    }
}
