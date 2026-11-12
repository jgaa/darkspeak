import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root
    title: qsTr("Events")
    property var selectedEvent: null
    property bool wide: width >= 900 * appearance.factor
    onVisibleChanged: events.activate(visible)
    Component.onCompleted: events.activate(visible)
    Component.onDestruction: events.activate(false)
    FontLoader {
        id: eventIcons
        source: "qrc:/fonts/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf"
    }
    background: Rectangle {
        color: appearance.surface
    }
    header: ToolBar {
        Flow {
            width: parent.width
            spacing: appearance.smallSpacing
            Label {
                text: events.enabled ? qsTr("Events — recording enabled") : qsTr("Events — recording disabled")
                padding: appearance.spacing
                textFormat: Text.PlainText
            }
            ToolButton {
                text: qsTr("Latest")
                onClicked: {
                    root.selectedEvent = null;
                    events.latest();
                }
            }
            ToolButton {
                text: qsTr("Check chain")
                enabled: !events.verifying
                onClicked: events.verify()
            }
            ToolButton {
                objectName: "clearEvents"
                text: qsTr("Clear events…")
                onClicked: clearDialog.open()
            }
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appearance.spacing
        Label {
            Layout.fillWidth: true
            visible: events.error.length > 0
            text: events.error
            color: appearance.error
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: events.verification.length > 0
            text: events.verification
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: appearance.spacing
            ColumnLayout {
                Layout.minimumWidth: 0
                Layout.preferredWidth: root.width * 0.6
                visible: root.wide || !root.selectedEvent
                Layout.fillWidth: true
                Layout.fillHeight: true
                ListView {
                    id: list
                    objectName: "eventsList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: events
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        required property var event
                        width: list.width
                        height: 36 * appearance.factor
                        highlighted: root.selectedEvent !== null && root.selectedEvent.id === event.id
                        text: Qt.formatDateTime(event.time, Qt.DefaultLocaleShortDate) + "  " + event.title + " · " + event.flow + (event.result ? " · " + event.result : "") + (event.identity ? qsTr(" · Identity #%1").arg(event.identity) : "") + (event.contact ? qsTr(" · Contact #%1").arg(event.contact) : "") + (event.filename ? " · " + event.filename : "") + (event.repeat_count ? qsTr(" · %1 similar events").arg(event.repeat_count) : "")
                        onClicked: root.selectedEvent = Object.assign({}, event)
                        contentItem: Label {
                            text: parent.text
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: parent.highlighted ? appearance.selectedText : appearance.text
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: list.count === 0
                        text: qsTr("No recorded events")
                        color: appearance.secondaryText
                    }
                }
                RowLayout {
                    Button {
                        text: qsTr("Newer")
                        enabled: events.hasNewer
                        onClicked: {
                            root.selectedEvent = null;
                            events.newer();
                        }
                    }
                    Button {
                        text: qsTr("Older")
                        enabled: events.hasOlder
                        onClicked: {
                            root.selectedEvent = null;
                            events.older();
                        }
                    }
                }
            }
            Pane {
                objectName: "eventDetail"
                visible: root.wide || root.selectedEvent !== null
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: root.wide ? root.width * 0.4 : root.width
                background: Rectangle {
                    color: appearance.raised
                    border.color: appearance.border
                }
                ColumnLayout {
                    anchors.fill: parent
                    ToolButton {
                        objectName: "eventsBack"
                        visible: !root.wide
                        text: "arrow_back"
                        font.family: eventIcons.name
                        font.pixelSize: appearance.iconSize
                        font.features: {
                            "liga": 1
                        }
                        Accessible.name: qsTr("Back to events")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Back to events")
                        onClicked: root.selectedEvent = null
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        TextArea {
                            width: parent.width
                            readOnly: true
                            selectByMouse: true
                            textFormat: TextEdit.PlainText
                            wrapMode: TextEdit.WrapAnywhere
                            text: {
                                const e = root.selectedEvent;
                                if (!e)
                                    return qsTr("Select an event to view its details.");
                                let lines = [e.title, qsTr("Event #%1").arg(e.id), Qt.formatDateTime(e.time, Qt.DefaultLocaleLongDate), qsTr("Timestamp precision: %1 seconds").arg(e.precision), e.flow];
                                if (e.result)
                                    lines.push(e.result);
                                if (e.identity)
                                    lines.push(qsTr("Identity #%1").arg(e.identity));
                                if (e.contact)
                                    lines.push(qsTr("Contact #%1").arg(e.contact));
                                if (e.filename)
                                    lines.push(qsTr("Filename: %1").arg(e.filename));
                                if (e.object)
                                    lines.push(qsTr("Object ID: %1").arg(e.object));
                                if (e.repeat_count) {
                                    lines.push(qsTr("%1 similar events suppressed").arg(e.repeat_count));
                                    lines.push(qsTr("Window: %1 — %2").arg(Qt.formatDateTime(new Date(e.window_start * 1000), Qt.DefaultLocaleLongDate)).arg(Qt.formatDateTime(new Date(e.window_end * 1000), Qt.DefaultLocaleLongDate)));
                                }
                                lines.push(qsTr("Hash: %1").arg(e.hash), qsTr("Previous hash: %1").arg(e.previous_hash));
                                return lines.join("\n\n");
                            }
                        }
                    }
                }
            }
        }
    }
    UiDialog {
        id: clearDialog
        parent: Overlay.overlay
        objectName: "clearEventsDialog"
        title: qsTr("Clear all events?")
        width: Math.min(root.width - appearance.spacing * 2, 480 * appearance.factor)
        implicitHeight: 240 * appearance.factor
        height: Math.min(root.height - appearance.spacing * 2, implicitHeight)
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label {
            text: qsTr("This deletes the recorded events and starts a new hash chain with an Events cleared entry. Recording keeps its current setting.")
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
        }
        onAccepted: {
            if (events.clear())
                root.selectedEvent = null;
        }
    }
}
