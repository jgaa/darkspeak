import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    property var settings: settingsSession.values
    spacing: 10 * appearance.factor
    function commit() {
        settings.dbpath = database.text;
        settings.downloadLocation = download.text;
        settings["storage/originalFilenames"] = originalNames.checked;
        settings["storage/repairPermissions"] = repairPermissions.checked;
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
        text: qsTr("Managed downloads")
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
    CheckBox {
        id: repairPermissions
        text: qsTr("Automatically restore owner-only storage permissions")
        checked: settings["storage/repairPermissions"]
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Applies to managed storage owned by your user, as entries are opened. Other owners are always rejected. When disabled, incorrect permissions prevent access. Restart to apply.")
    }
    CheckBox {
        id: originalNames
        text: qsTr("Use original filenames for managed downloads")
        checked: settings["storage/originalFilenames"]
        enabled: !settings["storage/downloadsInitialized"] || download.text !== settings.downloadLocation
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Downloads use a Managed-v1 folder inside this directory. Filename policy is fixed when that storage is first created. Changing the location creates a separate store. Previously managed files remain accessible in their existing stores. Restart to apply storage changes.")
    }
}
