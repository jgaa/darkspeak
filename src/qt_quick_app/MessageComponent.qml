import QtQuick 2.0
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Item {
    property var messageColors: ["silver", "orange", "yellow", "lightgreen", "red"]
    function pickColor(direction, state, type) {
        if (typeof state !== 'number') {
            return "grey"
        }

        if (cData.direction === Message.INCOMING)
            return "lightblue"

        return messageColors[state]
    }

    x: cData.direction === Message.OUTGOING ? 0 : msgBorder
    width: cList.width - msgBorder - scrollBar.width
    height: textarea.contentHeight + (margin * 2) + date.height

    Rectangle {
        id: textbox
        property int margin: 4
        anchors.fill: parent
        color: pickColor(cData.direction, cData.messageState, cData.type)
        radius: 4
    }

    TextEdit {
        anchors.left: textbox.left
        anchors.top: textbox.top
        anchors.margins: 2
        font.pointSize: 8
        color: cData.direction === Message.INCOMING
            ? "darkblue" : "darkgreen"
        text: cData.stateName ? qsTr(cData.stateName) : ""
    }

    FormattedTime {
        id: date
        anchors.right: textbox.right
        anchors.top: textbox.top
        anchors.margins: 2
        font.pointSize: 8
        color: cData.direction === Message.INCOMING
            ? "darkblue" : "darkgreen"
        value: cData.composedTime
    }

    MouseArea {
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        anchors.fill: parent
        onClicked: {
            cList.currentIndex = index
        }

        onPressAndHold: {
            cList.currentIndex = index
        }
    }

    TextEdit {
        id: textarea
        anchors.top: date.bottom
        anchors.left: textbox.left
        anchors.right: textbox.right
        anchors.leftMargin: 2
        anchors.rightMargin: 2
        anchors.topMargin: margin
        readOnly: true
        selectByMouse: true
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        width: parent.width - (margin * 2)
        text: cData.content
        clip: true
    }

}
