import QtQuick
import QtQuick.Controls
import org.darkspeak 1.0

Item {
    property var messageColors: ["silver", "orange", "yellow", "lightgreen", "red"]

    function chatDate(value) {
        if (!value) {
            return ""
        }

        var date = new Date(value)
        if (isNaN(date.getTime())) {
            return ""
        }

        var now = new Date()
        var today = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        var messageDay = new Date(date.getFullYear(), date.getMonth(), date.getDate())
        var days = Math.round((today.getTime() - messageDay.getTime()) / 86400000)

        if (days === 0) {
            return qsTr("today") + " " + Qt.formatDateTime(date, "hh:mm")
        }
        if (days === 1) {
            return qsTr("yesterday") + " " + Qt.formatDateTime(date, "hh:mm")
        }

        var weekdays = [qsTr("sun"), qsTr("mon"), qsTr("tue"), qsTr("wed"),
                        qsTr("thu"), qsTr("fri"), qsTr("sat")]
        return weekdays[date.getDay()] + " " + Qt.formatDateTime(date, "yyyy-MM-dd hh:mm")
    }

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
    height: textarea.contentHeight + (margin * 2) + date.height + delivery.height

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

    Text {
        id: date
        anchors.right: textbox.right
        anchors.top: textbox.top
        anchors.margins: 2
        font.pointSize: 8
        color: cData.direction === Message.INCOMING
            ? "darkblue" : "darkgreen"
        text: chatDate(cData.composedTime)
    }

    Text {
        id: delivery
        anchors.right: textbox.right
        anchors.top: date.bottom
        anchors.rightMargin: 2
        font.pointSize: 7
        color: cData.direction === Message.INCOMING
            ? "darkblue" : "darkgreen"
        text: chatDate(cData.receivedTime)
              ? qsTr("delivered") + " " + chatDate(cData.receivedTime)
              : ""
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
        anchors.top: delivery.bottom
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
