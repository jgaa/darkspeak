import QtQuick 2.12
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Page {
    id: root
    //width: 600
    //height: 400
    property int input_height: 96
    property int msg_border: 16
    visible: conversations.current
    property var states: [qsTr("Composed"), qsTr("Queued"), qsTr("Sent"), qsTr("Received"), qsTr("Rejected")]
    property var scolors: ["silver", "orange", "yellow", "lightgreen", "red"]

    header: Label {
        text: conversations.current ? (qsTr("Chat with ") + conversations.current.name) : qsTr("No conversation selected")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }

    function getStateName(state) {
        return states[state]
    }

    function pickColor(direction, state) {
        if (direction === Message.INCOMING)
            return "lightblue"
        return scolors[state]
    }

    // Background
    Rectangle {
        width: parent.width
        height: parent.height
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#ffffff" }
            GradientStop { position: 1.0; color: "#d7d7d7" }
        }
    }


    ListView {
        id: list
        width: root.width
        anchors.bottom: input.top
        anchors.bottomMargin: 4
        anchors.top: parent.top
        anchors.topMargin: 4
        highlightRangeMode: ListView.NoHighlightRange
        //height: parent.height - input_height
        spacing: 4
        clip: true
        ScrollIndicator.vertical: ScrollIndicator {}
        model: messages

        // Scroll to the end
        onCountChanged: {
            if (currentIndex === -1 || currentIndex === (count -2)) {
                var newIndex = count - 1; // last index
                positionViewAtEnd();
                currentIndex = newIndex;
            }
        }

        delegate:
            Component {
                Rectangle {
                id: textbox
                property int margin: 4
                x: direction === Message.OUTGOING ? 0 : msg_border
                width: parent.width - msg_border
                height: textarea.contentHeight + (margin * 2) + date.height
                color: pickColor(direction, messageState)
                radius: 4

                TextEdit {
                    anchors.left: textbox.left
                    anchors.top: textbox.top
                    anchors.margins: 2
                    font.pointSize: 8
                    color: direction === Message.INCOMING
                        ? "darkblue" : "darkgreen"
                    //text: root.states[messageState] + ' ' + index
                    text: messageState
                }

                TextEdit {
                    id: date
                    anchors.right: textbox.right
                    anchors.top: textbox.top
                    anchors.margins: 2
                    font.pointSize: 8
                    color: direction === Message.INCOMING
                        ? "darkblue" : "darkgreen"
                    text: composedTime
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
                    //height: parent.height - (margin * 2)
                    text: content
                    clip: true
                }
            } // Rectangle
        } // Delegate

    } // ListView

    Rectangle {
        id: input
        width: parent.width
        height: input_height
        radius: 4
        clip: false
        anchors.bottom: parent.bottom
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
            color: "black"
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
                conversations.current.sendMessage(text)
                textinput.text = ""
            }
        }
    }

    Button {
        text: qsTr("Send")
        autoExclusive: false
        antialiasing: true
        smooth: true
        z: 4
        clip: false
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        icon.source: "qrc:/images/Sent-32.png"

        onClicked: {
            textinput.send()
        }
    }


}
