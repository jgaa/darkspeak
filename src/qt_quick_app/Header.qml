import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3

Item {
    id: root
    height: 50
    property var whom
    property var text

    Label {
        id: label
        text: root.text + (root.whom ? root.whom.name : "")
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: 4
        font.pixelSize: Qt.application.font.pixelSize * 2
        anchors.leftMargin: 4
    }

    RoundedImage {
        id: headerImage
        source: root.whom ? root.whom.avatarUrl : null
        height: root.height - 2
        width: root.height -2
        border.width: 2
        border.color: (root.whom && root.whom.online) ? "lime" : "firebrick"
        color: "black"
        anchors.left: label.right
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
    }
}
