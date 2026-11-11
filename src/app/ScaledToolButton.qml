import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T

T.Button {
    id: control
    implicitWidth: 28 * appearance.factor
    implicitHeight: 28 * appearance.factor
    padding: 4 * appearance.factor
    font: appearance.font
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    contentItem: Text {
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        color: control.enabled ? appearance.text : appearance.disabled
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        color: control.down || control.hovered ? appearance.hover : "transparent"
        radius: appearance.cornerRadius
        border.width: Math.max(0.5, appearance.factor)
        border.color: control.activeFocus ? appearance.accent : "transparent"
    }
}
