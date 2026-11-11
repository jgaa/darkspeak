import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

Item {
    id: root
    implicitHeight: content.implicitHeight

    property var settings: settingsSession.values

    function commit() {
        settings.appAutoConnect = autoConnect.checked;
        settings.useDarkSpeakFolder = useDarkSpeakFolder.checked;
        settings.autostart = autostart.checked;
        settings.autoAcceptIncomingFiles = autoAccept.checked;
        settings.includeConversationNameInDownloadPath = includeConversationName.checked;
    }

    ColumnLayout {
        id: content
        width: parent.width
        height: implicitHeight
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: appearance.smallSpacing

        CheckBox {
            id: autoConnect
            text: qsTr("Auto-connect to Tor")
            ToolTip.text: qsTr("Connect automatically when the application is started")
            ToolTip.visible: hovered
            checked: settings.appAutoConnect
        }

        CheckBox {
            id: autostart
            text: qsTr("Start automatically")
            ToolTip.text: qsTr("Start DarkSpeak minimized to the taskbar when you log in")
            ToolTip.visible: hovered
            checked: settings.autostart
        }

        CheckBox {
            id: useDarkSpeakFolder
            text: qsTr("Use DarkSpeak folder in home")
            ToolTip.text: qsTr("Store downloads and logs in ~/DarkSpeak")
            ToolTip.visible: hovered
            enabled: !desktopIntegration.darkSpeakFolderForced
            checked: settings.useDarkSpeakFolder || desktopIntegration.darkSpeakFolderForced
        }

        CheckBox {
            id: autoAccept
            text: qsTr("Automatically accept and download incoming files")
            ToolTip.text: qsTr("Download allowed files without asking. This does not open files or enable image or video previews.")
            ToolTip.visible: hovered
            checked: settings.autoAcceptIncomingFiles
        }

        CheckBox {
            id: includeConversationName
            text: qsTr("Use conversation name in download path")
            ToolTip.text: qsTr("Store incoming files in a folder named after the conversation")
            ToolTip.visible: hovered
            checked: settings.includeConversationNameInDownloadPath
        }
    }
}
