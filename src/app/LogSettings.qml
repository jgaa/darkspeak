import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import org.darkspeak 1.0

ScrollView {
    id: root
    clip: true

    Settings {
        id: settings
        property string logPath: ""
        property int logLevelFile: 0
        property int logLevelStdout: 0
        property int logLevelApp: 0
    }

    function commit() {
        settings.logPath = logPath.text
        settings.logLevelFile = fileLog.currentIndex
        settings.logLevelStdout = stdoutLog.currentIndex
        settings.logLevelApp = viewerLog.currentIndex
        settings.sync()
    }

    GridLayout {
        id: fields
        width: root.availableWidth
        columns: width >= 480 ? 2 : 1
        rowSpacing: 10
        columnSpacing: 16
        property var levels: [qsTr("Disabled"), qsTr("Error"), qsTr("Warning"),
            qsTr("Notice"), qsTr("Info"), qsTr("Debug"), qsTr("Trace")]

        Label { text: qsTr("Log file") }
        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: logPath
                Layout.fillWidth: true
                text: settings.logPath
                placeholderText: qsTr("Optional log file path")
            }
            Button {
                text: qsTr("Browse…")
                onClicked: {
                    const selected = fileDialogs.openFile(
                        qsTr("Select log file"), logPath.text)
                    if (selected)
                        logPath.text = selected
                }
            }
        }

        Label { text: qsTr("Stdout level") }
        ComboBox {
            id: stdoutLog
            Layout.fillWidth: true
            model: fields.levels
            currentIndex: settings.logLevelStdout
        }

        Label { text: qsTr("Log file level") }
        ComboBox {
            id: fileLog
            Layout.fillWidth: true
            model: fields.levels
            currentIndex: settings.logLevelFile
        }

        Label { text: qsTr("Log viewer level") }
        ComboBox {
            id: viewerLog
            Layout.fillWidth: true
            model: fields.levels
            currentIndex: settings.logLevelApp
        }

        Label {
            Layout.columnSpan: fields.columns
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("Logging settings are applied when DarkSpeak starts. The log viewer keeps the latest 1000 messages.")
        }
    }

}
