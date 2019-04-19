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
    property var messageColors: ["silver", "orange", "yellow", "lightgreen", "red"]
    property var fileColors: ["silver", "blue", "silver", "orange", "yellow", "yellowgreen", "lightgreen", "red", "red", "red"]

    header: Header {
        whom: conversations.current.participant
        text: qsTr("Chat with ")
        anchors.left: parent.left
        anchors.leftMargin: 4
        anchors.top: parent.top
        anchors.topMargin: 4
    }

    function pickColor(direction, state, type) {
        if (type === MessagesModel.FILE) {
            return fileColors[state]
        }

        if (direction === Message.INCOMING)
            return "lightblue"
        return messageColors[state]
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

        delegate:
            Component {
                Rectangle {
                id: textbox
                property File cfile: file
                property int margin: 4
                x: direction === Message.OUTGOING ? 0 : msg_border
                width: parent.width - msg_border - scrollbar.width
                height: type === MessagesModel.MESSAGE ? textarea.contentHeight + (margin * 2) + date.height : 48
                color: pickColor(direction, messageState, type)
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

                ProgressBar {
                    id: pbar
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.top: fileName.bottom
                    anchors.topMargin: 0
                    anchors.left: fileName.left
                    anchors.leftMargin: 0
                    visible: type === MessagesModel.FILE
                    from: 0.0
                    to: 1.0
                    value: cfile.progress
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

                        var ctxmenu = null
                        if (file.direction === File.OUTGOING) {
                            ctxmenu = contextFileSendMenu
                        } else {
                            ctxmenu = contextFileReceiveMenu
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
                || contextFileSendMenu.file.state === File.FS_OFFERED
            text: qsTr("Cancel")
        }

        MenuItem {
            onTriggered: {
                manager.textToClipboard(contextFileSendMenu.file.name + ": " + contextFileSendMenu.file.hash)
            }

            text: qsTr("Copy Sha256")
        }

        MenuItem {
            onTriggered: {
                contextFileSendMenu.file.openInDefaultApplication()
            }
            text: qsTr("Open File")
        }

        MenuItem {
            onTriggered: {
                contextFileSendMenu.file.openFolder()
            }
            text: qsTr("Open Folder")
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
            enabled: contextFileReceiveMenu.file.state == File.FS_OFFERED
        }

        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.reject();
            }

            text: contextFileReceiveMenu.file.state == File.FS_TRANSFERRING ? qsTr("Cancel") : qsTr("Reject")
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

        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.openInDefaultApplication()
            }
            text: qsTr("Open File (take care!)")
            enabled: contextFileReceiveMenu.file.state === File.FS_DONE
        }

        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.openFolder()
            }
            text: qsTr("Open Folder")
            enabled: contextFileReceiveMenu.file && contextFileReceiveMenu.file.state === File.FS_DONE
        }
    }
}



/*##^## Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
 ##^##*/
