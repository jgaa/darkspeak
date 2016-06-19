
import QtQuick 2.4
import QtQuick.Controls 1.3

import com.jgaa.darkspeak 1.0

Item {
    id: root
    property int input_height: 96
    property int msg_border: 16
    property alias model: msgs.model
    property string stateName: "chat"

    Component {
        id: messageDelegate

        Rectangle {
            property int margin: 4
            x: direction === ChatMessagesModel.MD_INCOMING ? msg_border : 0
            width: parent.width - msg_border
            height: textarea.contentHeight + (margin * 2)
            color: direction === ChatMessagesModel.MD_INCOMING ? "lightblue" : "lightgreen"
            radius: 4
            //border.color: direction === ChatMessagesModel.MD_INCOMING ? "blue" : "green"

            TextEdit {
                id: textarea
                x: margin
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
        radius: 8
        clip: false
        anchors.bottom: send.top
        anchors.bottomMargin: 2
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
                    if(event.key === Qt.Key_Enter) {
                        send()
                    }
                }
            }

            function send() {
                msgs.model.sendMessage(text)
                textinput.text = ""
            }
        }
    }

    Button {
        id: send
        anchors.right: root.right
        anchors.bottom: root.bottom
        iconSource: "qrc:/images/Sent-32.png"
        onClicked: {
            textinput.send()
        }
    }
}

