import QtQuick

Item {
    property int visits: 0
    Component.onCompleted: visits += 1
}
