import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import org.darkspeak 1.0

ApplicationWindow {
    id: mainWindow
    visible: true
    width: 1100
    height: 700
    minimumWidth: 760
    minimumHeight: 480
    title: Qt.application.displayName

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
                currentIndex: {
                    for (var i = 0; i < count; ++i) {
                        if (model.get(i).page === manager.currentPage)
                            return i
                    }
                    return -1
                }

                model: ListModel {
                    ListElement { shortName: "Home"; iconName: "home"; page: 3 }
                    ListElement { shortName: "IDs"; iconName: "badge"; page: 1 }
                    ListElement { shortName: "People"; iconName: "group"; page: 2 }
                    ListElement { shortName: "Conversations"; iconName: "forum"; page: 4 }
                    ListElement { shortName: "Log"; iconName: "description"; page: 0 }
                }

                onCurrentIndexChanged: {
                    if (currentIndex >= 0) {
                        var page = model.get(currentIndex).page
                        if (manager.currentPage !== page)
                            manager.currentPage = page
                    }
                }

                delegate: ItemDelegate {
                    width: navigation.width
                    height: 58
                    highlighted: ListView.isCurrentItem
                    hoverEnabled: true
                    onClicked: manager.currentPage = page

                    background: Rectangle {
                        radius: 6
                        color: parent.highlighted ? "#294d70"
                               : parent.hovered ? "cornflowerblue" : "transparent"
                        border.color: parent.highlighted ? "#79c2f2" : "transparent"
                        border.width: 1
                    }

                    contentItem: RowLayout {
                        spacing: 14

                        Label {
                            text: iconName
                            font.family: materialIcons.name
                            font.pixelSize: 30
                            font.features: { "liga": 1 }
                            color: parent.parent.highlighted ? "#e6f1fb"
                                   : parent.parent.hovered ? "#1e293b" : "#334155"
                            Layout.preferredWidth: 34
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Label {
                            text: qsTr(shortName)
                            font.pixelSize: 15
                            color: parent.parent.highlighted ? "#ffffff"
                                   : parent.parent.hovered ? "#1e293b" : "#243447"
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
                text: manager.statusMessage
                visible: text.length > 0
                color: manager.statusLevel === Manager.ERROR ? "#b42318"
                       : manager.statusLevel === Manager.WARNING ? "#9a6700"
                       : "#334155"
                elide: Text.ElideRight
                Layout.maximumWidth: 560
            }
        }
    }

    SettingsDialog { id: settingsDialog }
    AboutDialog { id: aboutDialog }
}
