import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    property alias title: titleLabel.text
    property alias description: descriptionLabel.text
    spacing: appearance.spacing
    Label {
        id: titleLabel
        Layout.fillWidth: true
        font: appearance.headingFont
        color: appearance.text
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }
    Label {
        id: descriptionLabel
        Layout.fillWidth: true
        color: appearance.secondaryText
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }
}
