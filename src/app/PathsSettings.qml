import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    property var settings: settingsSession.values
    spacing: 10 * appearance.factor
    function commit() {
        settings.dbpath = database.text;
        settings.downloadLocation = download.text;
    }
    Label {
        text: qsTr("Database")
    }
    RowLayout {
        Layout.fillWidth: true
        TextField {
            id: database
            text: settings.dbpath
            Layout.fillWidth: true
            Accessible.name: qsTr("Database path")
        }
        Button {
            text: qsTr("Browse…")
            onClicked: {
                const selected = fileDialogs.openFile(qsTr("Select database"), database.text);
                if (selected)
                    database.text = selected;
            }
        }
    }
    Label {
        text: qsTr("Downloads")
    }
    RowLayout {
        Layout.fillWidth: true
        TextField {
            id: download
            text: settings.downloadLocation
            Layout.fillWidth: true
            Accessible.name: qsTr("Download directory")
        }
        Button {
            text: qsTr("Browse…")
            onClicked: {
                const selected = fileDialogs.openFolder(qsTr("Download directory"), download.text);
                if (selected)
                    download.text = selected;
            }
        }
    }
}
