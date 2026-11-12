import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    spacing: appearance.spacing
    width: parent ? parent.width : implicitWidth
    CheckBox {
        id: enabledControl
        objectName: "eventsEnabled"
        text: qsTr("Record events")
        checked: settingsSession.values["events/enabled"]
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Records activity metadata, including identity and contact references. Message text, bios, file paths, and media contents are excluded. Existing events remain when recording is disabled.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: appearance.secondaryText
    }
    CheckBox {
        id: filenamesControl
        objectName: "eventsRecordFilenames"
        text: qsTr("Record filenames in file events")
        checked: settingsSession.values["events/recordFilenames"]
        enabled: enabledControl.checked
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("When enabled, future file events include the filename, without its path. Turning this off does not remove filenames from existing events.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: appearance.secondaryText
    }
    Label {
        text: qsTr("Event timestamp precision")
    }
    ComboBox {
        id: precisionControl
        objectName: "eventPrecision"
        Layout.fillWidth: true
        property var seconds: [0, 60, 300, 1800, 3600]
        model: [qsTr("Exact (seconds)"), qsTr("1 minute"), qsTr("5 minutes"), qsTr("30 minutes"), qsTr("1 hour")]
        currentIndex: Math.max(0, seconds.indexOf(settingsSession.values["events/precisionSeconds"]))
        Accessible.name: qsTr("Event timestamp precision")
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Timestamps are rounded upward before storage. Changes apply to future events. Events are kept until you clear them from the Events page. Recording pauses at 500,000 rows to limit disk use.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: appearance.secondaryText
    }
    function commit() {
        settingsSession.values["events/enabled"] = enabledControl.checked;
        settingsSession.values["events/recordFilenames"] = filenamesControl.checked;
        settingsSession.values["events/precisionSeconds"] = precisionControl.seconds[precisionControl.currentIndex];
    }
}
