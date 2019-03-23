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
    property var scolors: ["silver", "orange", "yellow", "lightgreen", "red"]

    header: Label {
        text: conversations.current ? (qsTr("Chat with ") + conversations.current.name) : qsTr("No conversation selected")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
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

        delegate:
            Component {
                Rectangle {
                id: textbox
                property File cfile: file
                property int margin: 4
                x: direction === Message.OUTGOING ? 0 : msg_border
                width: parent.width - msg_border - scrollbar.width
                height: type === MessagesModel.MESSAGE ? textarea.contentHeight + (margin * 2) + date.height : 48
                color: pickColor(direction, messageState)
                radius: 4

                TextEdit {
                    anchors.left: textbox.left
                    anchors.top: textbox.top
                    anchors.margins: 2
                    font.pointSize: 8
                    color: direction === Message.INCOMING
                        ? "darkblue" : "darkgreen"
                    text: qsTr(stateName)
                }

                TextEdit {
                    id: date
                    anchors.right: textbox.right
                    anchors.top: textbox.top
                    anchors.margins: 2
                    font.pointSize: 8
                    color: direction === Message.INCOMING
                        ? "darkblue" : "darkgreen"
                    text: new Date(composedTime).toLocaleString(Qt.locale(), "ddd yyyy-MM-dd hh:mm")
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
                    text: type === MessagesModel.MESSAGE ? content : null
                    clip: true
                    visible: type === MessagesModel.MESSAGE
                }

                Image {
                    id: icon
                    source: type === MessagesModel.FILE ? "qrc:/images/FileTansferActive.svg" : ""
                    visible: type === MessagesModel.FILE
                    height: 32
                    width: 32
                    anchors.top: date.bottom
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                }

                Label {
                    id: fileName
                    visible: type === MessagesModel.FILE
                    text: type === MessagesModel.FILE ? file.name : null
                    anchors.top: date.bottom
                    anchors.left: icon.right
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                    color: "black"
                }

                Label {
                    id: fileSize
                    visible: type === MessagesModel.FILE
                    text: type === MessagesModel.FILE ? file.size + " bytes" : null
                    anchors.top: date.bottom
                    anchors.left: fileName.right
                    color: "gray"
                    anchors.leftMargin: 6
                    anchors.rightMargin: 2
                }

                MouseArea {
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    anchors.fill: parent
                    onClicked: {
                        list.currentIndex = index
                        if (mouse.button === Qt.RightButton) {
                            showMenu(mouse)
                        }
                    }

                    onPressAndHold: {
                        list.currentIndex = index
                        showMenu(mouse)
                    }
                }

                function showMenu(mouse) {
                    if (type === MessagesModel.FILE) {

                        if (file.direction === File.OUTGOING) {
                            var ctxmenu = contextFileSendMenu
                        } else {
                            var ctxmenu = contextFileReceiveMenu
                        }

                        ctxmenu.x = mouse.x;
                        ctxmenu.y = mouse.y;
                        ctxmenu.file = file
                        ctxmenu.open();
                    }
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

    Menu {
        id: contextFileSendMenu
        property File file: null

        MenuItem {
            onTriggered: {
                contextFileSendMenu.file.cancel()
            }

            enabled : contextFileSendMenu.file.state === File.FS_WAITING
            text: qsTr("Cancel")
        }

        MenuItem {
            onTriggered: {
                manager.textToClipboard(contextFileSendMenu.file.name + ": " + contextFileSendMenu.file.hash)
            }

            text: qsTr("Copy Sha256")
        }
    }

    Menu {
        id: contextFileReceiveMenu
        property File file: null


        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.accept();
            }
            text: qsTr("Accept")
        }

        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.reject();
            }

            text: qsTr("Reject")
        }

        MenuItem {
            onTriggered: {
                manager.textToClipboard(contextFileReceiveMenu.file.name)
            }

            text: qsTr("Copy Name")
        }

        MenuItem {
            onTriggered: {
                manager.textToClipboard(contextFileReceiveMenu.file.name + ": " + contextFileReceiveMenu.file.hash)
            }

            text: qsTr("Copy Sha256")
        }
    }
}
