import QtQuick
import QtQuick.Controls

Label {
    id: badge
    property color statusColor: appearance.secondaryText
    color: statusColor
    textFormat: Text.PlainText
    padding: appearance.smallSpacing
    Accessible.name: text
    background: Rectangle {
        color: appearance.surface
        border.color: badge.statusColor
        radius: appearance.cornerRadius
    }
}
