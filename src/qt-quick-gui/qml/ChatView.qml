
import QtQuick 2.4
import QtQuick.Controls 1.3

import com.jgaa.darkspeak 1.0

Item {
    id: root
    property int input_height: 96
    property int msg_border: 16
    property alias model: msgs.model
    property string stateName: "chat"
    //property string buddyName:
    //property string buddyId:

    function getStatus(state) {
        if (state === ChatMessagesModel.MS_QUEUED)
            return "Queued"
        if (state === ChatMessagesModel.MS_SENT)
            return "Sent"
        return ""
    }

    function pickColor(direction, status) {
        if (direction === ChatMessagesModel.MD_INCOMING)
            return "lightblue"
        if (status === ChatMessagesModel.MS_QUEUED)
            return "pink"
        return "lightgreen"
    }

    Component {
        id: messageDelegate

        Rectangle {
            id: textbox
            property int margin: 4
            x: direction === ChatMessagesModel.MD_INCOMING ? 0 : msg_border
            width: parent.width - msg_border
            height: textarea.contentHeight + (margin * 2) + date.height
            color: pickColor(direction, status)
            radius: 4

            TextEdit {
                anchors.left: textbox.left
                anchors.top: textbox.top
                anchors.margins: 2
                font.pointSize: 8
                color: direction === ChatMessagesModel.MD_INCOMING
                    ? "darkblue" : "darkgreen"
                text: getStatus(status)
            }

            TextEdit {
                id: date
                anchors.right: textbox.right
                anchors.top: textbox.top
                anchors.margins: 2
                font.pointSize: 8
                color: direction === ChatMessagesModel.MD_INCOMING
                    ? "darkblue" : "darkgreen"
                text: timestamp
            }

            TextEdit {
                id: textarea
                anchors.top: date.bottom
                anchors.left: textbox.left
                anchors.right: textbox.right
                anchors.leftMargin: 2
                anchors.rightMargin: 2
                y: margin
                readOnly: true
                selectByMouse: true
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                width: parent.width - (margin * 2)
                height: parent.height - (margin * 2)
                text: content
                clip: true
            }
        }
    }

    // Background
    Rectangle {
        width: root.width
        height: root.height
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#ffffff" }
            GradientStop { position: 1.0; color: "#d7d7d7" }
        }
    }

    ListView {
        id: msgs
        width: root.width
        anchors.bottom: input.top
        anchors.bottomMargin: 4
        anchors.top: parent.top
        anchors.topMargin: 4
        highlightRangeMode: ListView.NoHighlightRange
        //height: parent.height - input_height
        spacing: 4
        clip: true
        delegate: messageDelegate
        onCountChanged: {
            console.log("onAddChanged: hit!")
            if (currentIndex === count - 2) {
                currentIndex = count - 1
            }
        }
        onModelChanged: {
            positionViewAtEnd()
            currentIndex = count - 1
        }
    }

    Rectangle {
        id: input
        width: root.width
        height: input_height
        radius: 4
        clip: false
        anchors.bottom: send.top
        //anchors.bottomMargin: 2
        anchors.margins: 4
        color: "white"
        border.color: "steelblue"
        border.width: 1

        TextArea {
            id: textinput
            anchors.fill: parent
            text: ""
            clip: true
            focus: true
            Keys.onPressed: {
                if(event.modifiers && Qt.ControlModifier) {
                    if((event.key === Qt.Key_Enter)
                        || (event.key === Qt.Key_Return)) {
                        event.accepted = true
                        textinput.send()
                    }
                }
            }

            function send() {
                msgs.model.sendMessage(text)
                textinput.text = ""
            }
        }
    }

    Label {
        anchors.left: root.left
        anchors.bottom: root.bottom
        anchors.margins: 2
        color: "navy"
        text: model.buddyName + " " + model.buddyId
    }

    Button {
        id: send
        anchors.right: root.right
        anchors.bottom: root.bottom
        anchors.margins: 2
        iconSource: "qrc:/images/Sent-32.png"
        onClicked: {
            textinput.send()
        }
    }
}

