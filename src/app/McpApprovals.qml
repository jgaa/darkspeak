import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root
    implicitHeight: content.implicitHeight
    clip: true
    contentWidth: availableWidth
    ColumnLayout {
        id: content
        width: root.availableWidth
        spacing: appearance.spacing
        Repeater {
            model: mcp ? mcp.pending : []
            Frame {
                id: card
                required property var modelData
                Layout.fillWidth: true
                ColumnLayout {
                    width: parent.width
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("MCP approval: %1 — %2 (%3 seconds)").arg(modelData.key).arg(modelData.tool).arg(modelData.remaining)
                        textFormat: Text.PlainText
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Identity: %1\nContacts: %2").arg(modelData.identity).arg(modelData.contacts.join(", "))
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                    }
                    Label {
                        text: qsTr("Complete arguments (JSON; formatting controls escaped)")
                        textFormat: Text.PlainText
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 130 * appearance.factor
                        TextArea {
                            text: modelData.arguments
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            wrapMode: TextEdit.WrapAnywhere
                            selectByMouse: true
                            Accessible.name: qsTr("Complete operation arguments")
                        }
                    }
                    ComboBox {
                        id: scope
                        model: [qsTr("Selected contacts"), qsTr("All contacts")]
                        Accessible.name: qsTr("Persistent decision scope")
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: appearance.smallSpacing
                        Repeater {
                            model: ["Allow once", "Allow always", "Deny", "Deny always"]
                            Button {
                                required property string modelData
                                text: modelData
                                onClicked: {
                                    mcp.decide(card.modelData.id, modelData, scope.currentIndex === 1 ? "All contacts" : "Selected contacts");
                                }
                            }
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: mcp ? mcp.error : ""
            textFormat: Text.PlainText
            visible: text.length > 0
            color: appearance.error
        }
    }
}
