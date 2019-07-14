import QtQuick 2.12
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Page {
    id: root
    property int input_height: 96
    property int msg_border: 16
    visible: conversations.current

    header: Header {
        whom: conversations.current ? conversations.current.participant : null
        text: qsTr("Chat with ") + whom ? whom.name : ""
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

    function scrollToEnd() {
        var newIndex = list.count - 1; // last index
        list.positionViewAtEnd();
        list.currentIndex = newIndex;
    }

    Connections {
        target: messages
        onModelReset: {
            scrollToEnd()
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
        spacing: 4
        clip: true
        ScrollBar.vertical: ScrollBar { id: scrollbar}
        model: messages

        // Scroll to the end
        onCountChanged: {
            if (currentIndex === -1 || currentIndex === (count -2)) {
                scrollToEnd()
            }
        }

        DropArea {
            anchors.fill: parent
            onDropped: {
                if (drop.hasUrls) {
                    console.log(drop.urls.length + ' files dropped in chat window')
                    for (var i = 0; i < drop.urls.length; ++i) {
                        var file = drop.urls[i]
                        const args = {
                            "path" : file
                        }

                        console.log('Sending fropped file: ' + file)
                        conversations.current.sendFile(args);
                    }
                }
            }
        }

        delegate: Component {
            Loader {
                property File cFile: file ? file : null
                property var cData: model
                property int msgBorder: msg_border
                property int margin: 4
                property var cList: list
                property var scrollBar: scrollbar
                source:
                    switch(type) {
                    case MessagesModel.MESSAGE: return "MessageComponent.qml"
                    case MessagesModel.FILE: return "FileComponent.qml"
                }
            }
        }
    }

    Rectangle {
        id: input
        width: parent.width
        height: input_height
        radius: 4
        clip: false
        anchors.bottom: parent.bottom
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

