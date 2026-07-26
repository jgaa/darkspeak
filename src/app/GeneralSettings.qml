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
    }

    function commit() {
        settings.appAutoConnect = autoConnect.checked
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
    }

}
