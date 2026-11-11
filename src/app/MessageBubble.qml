import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: bubble
    property string messageText
    property string timestamp
    property string status
    property bool outgoing: false
    property bool failed: false
    implicitHeight: contents.implicitHeight + 20 * appearance.factor
    radius: appearance.bubbleRadius
    color: outgoing ? appearance.outgoing : appearance.incoming
    border.color: appearance.border
    ColumnLayout {
        id: contents
        x: 10 * appearance.factor
        y: 10 * appearance.factor
        width: parent.width - 20 * appearance.factor
        spacing: 5 * appearance.factor
        TextEdit {
            Layout.fillWidth: true
            text: bubble.messageText
            textFormat: TextEdit.PlainText
            color: appearance.text
            font: appearance.font
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            Accessible.name: text
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: bubble.timestamp
                textFormat: Text.PlainText
                color: appearance.secondaryText
                font: appearance.captionFont
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: bubble.status
                textFormat: Text.PlainText
                color: bubble.failed ? appearance.error : appearance.secondaryText
                font: appearance.captionFont
            }
        }
    }
}
