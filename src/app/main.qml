import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import com.jgaa.darkspeak 1.0

ApplicationWindow {
    id: mainWindow
    visible: true
    width: 1100
    height: 700
    minimumWidth: 760
    minimumHeight: 480
    title: manager.programNameAndVersion

    FontLoader {
        id: materialIcons
        source: "qrc:/fonts/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf"
    }

    Settings {
        category: "ApplicationWindow"
        property alias x: mainWindow.x
        property alias y: mainWindow.y
        property alias width: mainWindow.width
        property alias height: mainWindow.height
    }

    Settings {
        id: appSettings
        property bool appAutoConnect: true
    }

    function openSettings() {
        settingsDialog.open()
    }

    function openAbout() {
        aboutDialog.open()
    }

    Component.onCompleted: {
        if (appSettings.appAutoConnect) {
            manager.goOnline()
        }
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("File")

            Action {
                text: qsTr("Settings")
                shortcut: StandardKey.Preferences
                onTriggered: mainWindow.openSettings()
            }
            MenuSeparator {}
            Action {
                text: qsTr("Quit")
                shortcut: StandardKey.Quit
                onTriggered: Qt.quit()
            }
        }

        Menu {
            title: qsTr("Help")
            Action {
                text: qsTr("About DarkSpeak")
                onTriggered: mainWindow.openAbout()
            }
        }
    }

    header: MainToolBar {
        height: 38
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            id: navigationPane
            Layout.fillHeight: true
            Layout.preferredWidth: 190
            Layout.minimumWidth: 150
            padding: 8

            background: Rectangle {
                color: Qt.darker(palette.window, 1.08)
                border.color: palette.mid
                border.width: 1
            }

            ListView {
                id: navigation
                anchors.fill: parent
                clip: true
                spacing: 4
                currentIndex: manager.currentPage

                model: ListModel {
                    ListElement { shortName: "Log"; iconName: "description"; page: 0 }
                    ListElement { shortName: "IDs"; iconName: "badge"; page: 1 }
                    ListElement { shortName: "People"; iconName: "group"; page: 2 }
                    ListElement { shortName: "Home"; iconName: "home"; page: 3 }
                    ListElement { shortName: "Threads"; iconName: "forum"; page: 4 }
                    ListElement { shortName: "Chat"; iconName: "chat"; page: 5 }
                }

                onCurrentIndexChanged: {
                    if (manager.currentPage !== currentIndex)
                        manager.currentPage = currentIndex
                }

                delegate: ItemDelegate {
                    width: navigation.width
                    height: 58
                    highlighted: ListView.isCurrentItem
                    onClicked: navigation.currentIndex = page

                    contentItem: RowLayout {
                        spacing: 14

                        Label {
                            text: iconName
                            font.family: materialIcons.name
                            font.pixelSize: 30
                            font.features: { "liga": 1 }
                            Layout.preferredWidth: 34
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Label {
                            text: qsTr(shortName)
                            font.pixelSize: 15
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        StackLayout {
            id: pages
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: manager.currentPage

            Log {}
            Identities {}
            Contacts {}
            Home {}
            Conversations {}
            Chat {}
        }
    }

    footer: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8

            Image {
                source: manager.onlineStatusIcon
                sourceSize: Qt.size(18, 18)
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
            }

            Label {
                text: {
                    switch (manager.onlineState) {
                    case 1: return qsTr("Tor: Connecting")
                    case 2: return qsTr("Tor: Connected")
                    case 3: return qsTr("Tor: Online")
                    case 4: return qsTr("Tor: Disconnecting")
                    default: return qsTr("Tor: Offline")
                    }
                }
            }

            Label {
                visible: manager.torVersion.length > 0
                text: qsTr("(%1)").arg(manager.torVersion)
                opacity: 0.75
            }

            Item { Layout.fillWidth: true }

            Label {
                text: qsTr("Page %1 of %2").arg(manager.currentPage + 1).arg(pages.count)
                opacity: 0.7
            }
        }
    }

    SettingsDialog { id: settingsDialog }
    AboutDialog { id: aboutDialog }
}
