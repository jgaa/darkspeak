import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import org.darkspeak 1.0

ApplicationWindow {
    id: mainWindow
    visible: true
    width: 1366
    height: 768
    minimumWidth: Math.min(420, Screen.desktopAvailableWidth)
    minimumHeight: Math.min(320, Screen.desktopAvailableHeight)
    title: Qt.application.displayName
    // Bind roles individually: Popup copies whole Palette objects on assignment.
    palette.active.alternateBase: appearance.paletteColors.active.alternateBase
    palette.active.base: appearance.paletteColors.active.base
    palette.active.brightText: appearance.paletteColors.active.brightText
    palette.active.button: appearance.paletteColors.active.button
    palette.active.buttonText: appearance.paletteColors.active.buttonText
    palette.active.dark: appearance.paletteColors.active.dark
    palette.active.highlight: appearance.paletteColors.active.highlight
    palette.active.highlightedText: appearance.paletteColors.active.highlightedText
    palette.active.light: appearance.paletteColors.active.light
    palette.active.link: appearance.paletteColors.active.link
    palette.active.linkVisited: appearance.paletteColors.active.linkVisited
    palette.active.mid: appearance.paletteColors.active.mid
    palette.active.midlight: appearance.paletteColors.active.midlight
    palette.active.shadow: appearance.paletteColors.active.shadow
    palette.active.text: appearance.paletteColors.active.text
    palette.active.toolTipBase: appearance.paletteColors.active.toolTipBase
    palette.active.toolTipText: appearance.paletteColors.active.toolTipText
    palette.active.window: appearance.paletteColors.active.window
    palette.active.windowText: appearance.paletteColors.active.windowText
    palette.active.placeholderText: appearance.paletteColors.active.placeholderText
    palette.active.accent: appearance.paletteColors.active.accent
    palette.inactive.alternateBase: appearance.paletteColors.inactive.alternateBase
    palette.inactive.base: appearance.paletteColors.inactive.base
    palette.inactive.brightText: appearance.paletteColors.inactive.brightText
    palette.inactive.button: appearance.paletteColors.inactive.button
    palette.inactive.buttonText: appearance.paletteColors.inactive.buttonText
    palette.inactive.dark: appearance.paletteColors.inactive.dark
    palette.inactive.highlight: appearance.paletteColors.inactive.highlight
    palette.inactive.highlightedText: appearance.paletteColors.inactive.highlightedText
    palette.inactive.light: appearance.paletteColors.inactive.light
    palette.inactive.link: appearance.paletteColors.inactive.link
    palette.inactive.linkVisited: appearance.paletteColors.inactive.linkVisited
    palette.inactive.mid: appearance.paletteColors.inactive.mid
    palette.inactive.midlight: appearance.paletteColors.inactive.midlight
    palette.inactive.shadow: appearance.paletteColors.inactive.shadow
    palette.inactive.text: appearance.paletteColors.inactive.text
    palette.inactive.toolTipBase: appearance.paletteColors.inactive.toolTipBase
    palette.inactive.toolTipText: appearance.paletteColors.inactive.toolTipText
    palette.inactive.window: appearance.paletteColors.inactive.window
    palette.inactive.windowText: appearance.paletteColors.inactive.windowText
    palette.inactive.placeholderText: appearance.paletteColors.inactive.placeholderText
    palette.inactive.accent: appearance.paletteColors.inactive.accent
    palette.disabled.alternateBase: appearance.paletteColors.disabled.alternateBase
    palette.disabled.base: appearance.paletteColors.disabled.base
    palette.disabled.brightText: appearance.paletteColors.disabled.brightText
    palette.disabled.button: appearance.paletteColors.disabled.button
    palette.disabled.buttonText: appearance.paletteColors.disabled.buttonText
    palette.disabled.dark: appearance.paletteColors.disabled.dark
    palette.disabled.highlight: appearance.paletteColors.disabled.highlight
    palette.disabled.highlightedText: appearance.paletteColors.disabled.highlightedText
    palette.disabled.light: appearance.paletteColors.disabled.light
    palette.disabled.link: appearance.paletteColors.disabled.link
    palette.disabled.linkVisited: appearance.paletteColors.disabled.linkVisited
    palette.disabled.mid: appearance.paletteColors.disabled.mid
    palette.disabled.midlight: appearance.paletteColors.disabled.midlight
    palette.disabled.shadow: appearance.paletteColors.disabled.shadow
    palette.disabled.text: appearance.paletteColors.disabled.text
    palette.disabled.toolTipBase: appearance.paletteColors.disabled.toolTipBase
    palette.disabled.toolTipText: appearance.paletteColors.disabled.toolTipText
    palette.disabled.window: appearance.paletteColors.disabled.window
    palette.disabled.windowText: appearance.paletteColors.disabled.windowText
    palette.disabled.placeholderText: appearance.paletteColors.disabled.placeholderText
    palette.disabled.accent: appearance.paletteColors.disabled.accent
    font: appearance.font
    color: appearance.window

    FontLoader {
        id: materialIcons
        source: "qrc:/fonts/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf"
    }
    Settings {
        id: appSettings
        property bool appAutoConnect: true
    }

    function openSettings() {
        if (!settingsLoader.active)
            settingsLoader.active = true;
        else if (settingsLoader.item)
            settingsLoader.item.open();
    }
    function openAbout() {
        if (!aboutLoader.active)
            aboutLoader.active = true;
        else if (aboutLoader.item)
            aboutLoader.item.open();
    }
    Component.onCompleted: {
        appearance.restoreWindow(mainWindow);
        if (appSettings.appAutoConnect)
            Qt.callLater(manager.goOnline);
    }
    onClosing: appearance.saveWindow(mainWindow)
    Shortcut {
        sequences: ["Ctrl++", "Ctrl+="]
        onActivated: appearance.zoom(1)
    }
    Shortcut {
        sequence: "Ctrl+-"
        onActivated: appearance.zoom(-1)
    }
    Shortcut {
        sequence: "Ctrl+0"
        onActivated: appearance.scale = 100
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
            title: qsTr("View")
            Menu {
                title: qsTr("Scale")
                Action {
                    text: qsTr("Zoom in")
                    onTriggered: appearance.zoom(1)
                }
                Action {
                    text: qsTr("Zoom out")
                    onTriggered: appearance.zoom(-1)
                }
                Action {
                    text: qsTr("Reset to 100%")
                    onTriggered: appearance.scale = 100
                }
                MenuSeparator {}
                Repeater {
                    model: [20, 50, 75, 100, 125, 150, 200]
                    MenuItem {
                        required property int modelData
                        text: modelData + "%"
                        checkable: true
                        checked: appearance.scale === modelData
                        onTriggered: appearance.scale = modelData
                    }
                }
            }
            Menu {
                title: qsTr("Window Size")
                Action {
                    text: qsTr("1024×768")
                    onTriggered: appearance.resizeWindow(mainWindow, 1024, 768)
                }
                Action {
                    text: qsTr("1366×768 (Standard)")
                    onTriggered: appearance.resizeWindow(mainWindow, 1366, 768)
                }
                Action {
                    text: qsTr("1280×720 (HD)")
                    onTriggered: appearance.resizeWindow(mainWindow, 1280, 720)
                }
                Action {
                    text: qsTr("1920×1080 (Full HD)")
                    onTriggered: appearance.resizeWindow(mainWindow, 1920, 1080)
                }
                Action {
                    text: qsTr("2560×1440 (QHD)")
                    onTriggered: appearance.resizeWindow(mainWindow, 2560, 1440)
                }
                Action {
                    text: qsTr("Custom…")
                    onTriggered: customSize.active = true
                }
            }
            Action {
                text: qsTr("Collapse navigation")
                checkable: true
                checked: appearance.sidebarCollapsed
                onTriggered: appearance.sidebarCollapsed = !appearance.sidebarCollapsed
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
    header: MainToolBar {}
    RowLayout {
        anchors.fill: parent
        spacing: 0
        Pane {
            id: navigationPane
            property bool collapsed: appearance.sidebarCollapsed || mainWindow.width < 720 * appearance.factor
            Layout.fillHeight: true
            Layout.preferredWidth: (collapsed ? 56 : 184) * appearance.factor
            padding: 6 * appearance.factor
            background: Rectangle {
                color: appearance.sidebar
                border.color: appearance.border
            }
            ColumnLayout {
                anchors.fill: parent
                ScaledToolButton {
                    objectName: "navigationToggle"
                    Layout.alignment: Qt.AlignLeft
                    implicitWidth: 36 * appearance.factor
                    implicitHeight: 36 * appearance.factor
                    text: navigationPane.collapsed ? "menu" : "menu_open"
                    font.family: materialIcons.name
                    font.pixelSize: appearance.iconSize
                    font.features: {
                        "liga": 1
                    }
                    Accessible.name: navigationPane.collapsed ? qsTr("Expand navigation") : qsTr("Collapse navigation")
                    onClicked: appearance.sidebarCollapsed = !appearance.sidebarCollapsed
                    ToolTip.visible: hovered
                    ToolTip.text: navigationPane.collapsed ? qsTr("Expand navigation") : qsTr("Collapse navigation")
                }
                ListView {
                    id: navigation
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: appearance.smallSpacing
                    model: [
                        {
                            label: qsTr("Home"),
                            glyph: "home",
                            page: Manager.HOME
                        },
                        {
                            label: qsTr("Identities"),
                            glyph: "badge",
                            page: Manager.IDENTITIES
                        },
                        {
                            label: qsTr("People"),
                            glyph: "group",
                            page: Manager.CONTACTS
                        },
                        {
                            label: qsTr("Conversations"),
                            glyph: "forum",
                            page: Manager.CONVERSATIONS
                        },
                        {
                            label: qsTr("Log"),
                            glyph: "description",
                            page: Manager.LOG
                        }
                    ]
                    delegate: ItemDelegate {
                        id: navItem
                        required property var modelData
                        width: navigation.width
                        height: appearance.navigationHeight
                        highlighted: manager.currentPage === modelData.page
                        onClicked: manager.currentPage = modelData.page
                        Accessible.name: modelData.label
                        ToolTip.visible: hovered && navigationPane.collapsed
                        ToolTip.text: modelData.label
                        background: Rectangle {
                            radius: 5 * appearance.factor
                            color: navItem.highlighted ? appearance.selected : navItem.hovered ? appearance.hover : "transparent"
                            border.color: navItem.activeFocus ? appearance.accent : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: 10 * appearance.factor
                            Label {
                                text: navItem.modelData.glyph
                                font.family: materialIcons.name
                                font.pixelSize: appearance.iconSize
                                font.features: {
                                    "liga": 1
                                }
                                color: navItem.highlighted ? appearance.selectedText : appearance.text
                                Layout.preferredWidth: 30 * appearance.factor
                                horizontalAlignment: Text.AlignHCenter
                            }
                            Label {
                                visible: !navigationPane.collapsed
                                text: navItem.modelData.label
                                textFormat: Text.PlainText
                                color: navItem.highlighted ? appearance.selectedText : appearance.text
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            PrimaryPage {
                anchors.fill: parent
                pageNumber: Manager.HOME
                pageSource: "Home.qml"
            }
            PrimaryPage {
                anchors.fill: parent
                pageNumber: Manager.IDENTITIES
                pageSource: "Identities.qml"
            }
            PrimaryPage {
                anchors.fill: parent
                pageNumber: Manager.CONTACTS
                pageSource: "Contacts.qml"
            }
            PrimaryPage {
                anchors.fill: parent
                pageNumber: Manager.CONVERSATIONS
                pageSource: "Conversations.qml"
            }
            PrimaryPage {
                anchors.fill: parent
                pageNumber: Manager.LOG
                pageSource: "Log.qml"
            }
        }
    }
    footer: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.margins: 6 * appearance.factor
            spacing: 10 * appearance.factor
            Label {
                text: manager.onlineState === 1 ? qsTr("Tor: Connecting") : manager.onlineState === 2 ? qsTr("Tor: Connected") : manager.onlineState === 3 ? qsTr("Tor: Online") : manager.onlineState === 4 ? qsTr("Tor: Disconnecting") : qsTr("Tor: Offline")
                ToolTip.visible: torHover.hovered
                ToolTip.text: qsTr("Transport connectivity. This does not verify a contact's identity.") + "\n" + manager.torVersion
                HoverHandler {
                    id: torHover
                }
            }
            Label {
                visible: !!identities.current
                text: identities.current ? qsTr("Identity: %1").arg(identities.current.name) : ""
                textFormat: Text.PlainText
                elide: Text.ElideRight
                Layout.maximumWidth: mainWindow.width * .25
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                text: manager.statusMessage
                textFormat: Text.PlainText
                visible: text.length > 0
                color: manager.statusLevel === Manager.ERROR ? appearance.error : manager.statusLevel === Manager.WARNING ? appearance.warning : appearance.text
                elide: Text.ElideRight
                Layout.maximumWidth: mainWindow.width * .4
                ToolTip.visible: statusHover.hovered
                ToolTip.text: text
                HoverHandler {
                    id: statusHover
                }
            }
        }
    }
    Loader {
        id: settingsLoader
        active: false
        source: "SettingsDialog.qml"
        onLoaded: item.open()
        Connections {
            target: settingsLoader.item
            function onClosed() {
                settingsLoader.active = false;
            }
        }
    }
    Loader {
        id: aboutLoader
        active: false
        source: "AboutDialog.qml"
        onLoaded: item.open()
        Connections {
            target: aboutLoader.item
            function onClosed() {
                aboutLoader.active = false;
            }
        }
    }
    Loader {
        id: customSize
        active: false
        source: "CustomWindowSizeDialog.qml"
        onLoaded: item.open()
        Connections {
            target: customSize.item
            function onClosed() {
                customSize.active = false;
            }
        }
    }
}
