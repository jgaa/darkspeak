import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

Item {
    id: root
    implicitHeight: content.implicitHeight

    Settings {
        id: settings
        property bool appAutoConnect : true
        property bool autostart : false
        property bool autoAcceptIncomingFiles : false
        property bool includeConversationNameInDownloadPath : false
        property int maxIncomingFileSizeMb : 1024
        property int minimumFreeSpacePercent : 10
    }

    function commit() {
        settings.appAutoConnect = autoConnect.checked
        if (desktopIntegration.setAutostartEnabled(autostart.checked)) {
            settings.autostart = autostart.checked
        } else {
            autostart.checked = settings.autostart
        }
        settings.autoAcceptIncomingFiles = autoAccept.checked
        settings.includeConversationNameInDownloadPath = includeConversationName.checked
        settings.maxIncomingFileSizeMb = maxIncomingSize.value
        settings.minimumFreeSpacePercent = minimumFreeSpace.value
    }

    ColumnLayout {
        id: content
        width: parent.width
        height: implicitHeight
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 4

        CheckBox {
            id: autoConnect
            text: qsTr("Auto-connect")
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
            id: autoAccept
            text: qsTr("Auto-accept incoming files")
            ToolTip.text: qsTr("Start receiving files without asking first")
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

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Maximum incoming file size (MB)")
                Layout.fillWidth: true
            }

            SpinBox {
                id: maxIncomingSize
                from: 1
                to: 102400
                value: settings.maxIncomingFileSizeMb
                editable: true
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Minimum free space (%)")
                Layout.fillWidth: true
            }

            SpinBox {
                id: minimumFreeSpace
                from: 0
                to: 100
                value: settings.minimumFreeSpacePercent
                editable: true
            }
        }
    }

}
