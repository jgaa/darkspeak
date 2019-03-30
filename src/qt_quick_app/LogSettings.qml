import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import Qt.labs.settings 1.0
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Item {
    id: root

    Settings {
        id: settings
        property int logLevelFile: 0
        property string logPath: ""

        property int logLevelApp: 4
        property int logLevelStdout: 3
    }

    function commit() {
        settings.logLevelFile = fileLog.currentIndex
        settings.logPath = logPath.text
        settings.logLevelApp = appLog.currentIndex
        settings.logLevelStdout = debugLog.currentIndex
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        GridLayout {
            id: fields
            Layout.fillWidth: true
            rowSpacing: 4
            rows: 4
            flow: GridLayout.TopToBottom
            property var levels: [qsTr("Disabled"), qsTr("ERROR"), qsTr("WARNINGS"), qsTr("NOTICE"), qsTr("INFO"), qsTr("DEBUGGING"), qsTr("TRACE")]

            Label { font.pointSize: 9; text: qsTr("Log File")}
            Label { font.pointSize: 9; text: qsTr("Log Path")}
            Label { font.pointSize: 9; text: qsTr("App Log")}
            Label { font.pointSize: 9; text: qsTr("Debug log")}

            ComboBox {
                id: fileLog
                model: fields.levels
                Layout.fillWidth: true
                currentIndex: settings.logLevelFile
            }

            TextField {
                id: logPath
                text: settings.logPath
                Layout.fillWidth: true
            }

            ComboBox {
                id: appLog
                model: fields.levels
                Layout.fillWidth: true
                currentIndex: settings.logLevelApp
            }

            ComboBox {
                id: debugLog
                model: fields.levels
                Layout.fillWidth: true
                currentIndex: settings.logLevelStdout
            }
        }
    }
}
