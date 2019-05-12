import QtQuick 2.2
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import Qt.labs.settings 1.0

Item {
    id: root

    Settings {
        id: settings
        property bool appAutoConnect : true
    }

    function commit() {
        settings.appAutoConnect = autoConnect.checked
    }

    ColumnLayout {
        anchors.fill: parent
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
