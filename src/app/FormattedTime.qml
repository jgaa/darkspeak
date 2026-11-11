import QtQuick

TextEdit {
    property var value: null
    readOnly: true
    selectByMouse: true
    textFormat: TextEdit.PlainText
    font: appearance.captionFont
    color: appearance.secondaryText
    text: value ? Qt.formatDateTime(value, Qt.DefaultLocaleShortDate) : ""
}
