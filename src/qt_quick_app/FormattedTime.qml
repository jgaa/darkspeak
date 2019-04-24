import QtQuick 2.0

TextEdit {
    property var value: null
    text: new Date(value).toLocaleString(Qt.locale(), "ddd yyyy-MM-dd hh:mm")
}
