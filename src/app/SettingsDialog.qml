import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

UiDialog {
    id: root
    title: qsTr("Settings")
    width: Math.min(mainWindow.width - 24 * appearance.factor, 900 * appearance.factor)
    height: Math.min(mainWindow.height - 24 * appearance.factor, 760 * appearance.factor)
    property bool saved: false
    property int initialSection: 0

    Component.onCompleted: settingsSession.begin()
    Component.onDestruction: {
        if (!saved)
            settingsSession.cancel();
    }

    contentItem: ColumnLayout {
        spacing: 12 * appearance.factor
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ListView {
                id: sections
                objectName: "settingsSections"
                Layout.preferredWidth: 150 * appearance.factor
                Layout.fillHeight: true
                currentIndex: root.initialSection
                model: [qsTr("General"), qsTr("Appearance"), qsTr("Tor"), qsTr("Files / Paths"), qsTr("Logging"), qsTr("Limits"), qsTr("Events")].concat(mcp ? [qsTr("MCP")] : [])
                delegate: ItemDelegate {
                    required property int index
                    required property string modelData
                    width: ListView.view.width
                    text: modelData
                    highlighted: ListView.isCurrentItem
                    onClicked: sections.currentIndex = index
                }
            }
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: sections.currentIndex
                Repeater {
                    model: ["GeneralSettings.qml", "AppearanceSettings.qml", "TorSettings.qml", "PathsSettings.qml", "LogSettings.qml", "ResourceLimitsSettings.qml", "EventsSettings.qml"].concat(mcp ? ["McpSettings.qml"] : [])
                    ScrollView {
                        id: scroll
                        required property int index
                        required property string modelData
                        property bool visited: false
                        Component.onCompleted: {
                            if (sections.currentIndex === index)
                                visited = true;
                        }
                        Connections {
                            target: sections
                            function onCurrentIndexChanged() {
                                if (sections.currentIndex === scroll.index)
                                    scroll.visited = true;
                            }
                        }
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        Loader {
                            id: loader
                            width: scroll.availableWidth
                            active: scroll.visited
                            source: scroll.modelData
                            // Keep visited forms alive; stage their controls only on Save.
                            Connections {
                                target: root
                                function onStage() {
                                    if (loader.item && loader.item.commit)
                                        loader.item.commit();
                                }
                            }
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: settingsSession.error
            textFormat: Text.PlainText
            color: appearance.error
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Tor, storage, logging, and connection limits may require reconnection or restart.")
            color: appearance.secondaryText
            wrapMode: Text.WordWrap
        }
    }
    signal stage
    footer: DialogButtonBox {
        Button {
            objectName: "saveSettingsButton"
            text: qsTr("Save")
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                root.stage();
                if (settingsSession.save()) {
                    root.saved = true;
                    root.close();
                }
            }
        }
        Button {
            text: qsTr("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        onRejected: root.reject()
    }
}
