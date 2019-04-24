import QtQuick 2.0
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Item {
    property var fileColors: ["silver", "blue", "silver", "orange", "yellow", "yellowgreen", "lightgreen", "red", "red", "red"]

    function pickColor(file) {
        if (typeof file.state !== 'number') {
            return "grey"
        }

        return fileColors[file.state]
    }

    function getTransferHeight(file) {
        var height = 48

        if (file.direction === File.INCOMING && file.state === File.FS_OFFERED) {
            height += 32
        }

        return height
    }

    function showMenu(mouse) {
        var ctxmenu = null
        if (cFile.direction === File.OUTGOING) {
            ctxmenu = contextFileSendMenu
        } else {
            ctxmenu = contextFileReceiveMenu
        }

        ctxmenu.x = mouse.x;
        ctxmenu.y = mouse.y;
        ctxmenu.file = cFile
        ctxmenu.open();
    }

    x: cData.direction === Message.OUTGOING ? 0 : msgBorder
    width: cList.width - msgBorder - scrollBar.width
    height: getTransferHeight(cFile)

    Rectangle {
        id: background
        property int margin: 4
        anchors.fill: parent
        color: pickColor(cFile)
        radius: 4
    }

    TextEdit {
        anchors.left: background.left
        anchors.top: background.top
        anchors.margins: 2
        font.pointSize: 8
        color: cData.direction === Message.INCOMING
            ? "darkblue" : "darkgreen"
        text: cData.stateName ? qsTr(cData.stateName) : ""
    }

    FormattedTime {
        id: date
        anchors.right: background.right
        anchors.top: background.top
        anchors.margins: 2
        font.pointSize: 8
        color: cData.direction === Message.INCOMING
            ? "darkblue" : "darkgreen"
        value: cData.composedTime
    }

    Image {
        id: icon
        source: "qrc:/images/FileTansferActive.svg"
        height: 32
        width: 32
        anchors.top: date.bottom
        anchors.leftMargin: 2
        anchors.rightMargin: 2
    }

    Label {
        id: fileName
        text: cFile.name
        anchors.top: date.bottom
        anchors.left: icon.right
        anchors.leftMargin: 2
        anchors.rightMargin: 2
        color: "black"
    }

    Label {
        id: fileSize
        text: cFile.size + ' ' + qsTr("bytes")
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
        from: 0.0
        to: 1.0
        value: cFile.progress
    }

    MouseArea {
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        anchors.fill: parent
        onClicked: {
            cList.currentIndex = index
            if (mouse.button === Qt.RightButton) {
                showMenu(mouse)
            }
        }

        onPressAndHold: {
            cList.currentIndex = index
            showMenu(mouse)
        }
    }

    Button {
        id: accepptBtn
        height: 28
        text: qsTr("Accept")
        anchors.top: fileName.bottom
        anchors.topMargin: 4
        anchors.left: fileName.left
        anchors.leftMargin: 0
        visible:  (cFile.direction === File.INCOMING) && (cFile.state === File.FS_OFFERED)
        onClicked: {
            cFile.accept()
        }
    }

    Button {
        height: 28
        text: qsTr("Reject")
        anchors.top: fileName.bottom
        anchors.topMargin: 4
        anchors.left: accepptBtn.right
        anchors.leftMargin: 6
        visible: cFile.direction === File.INCOMING && cFile.state === File.FS_OFFERED
        onClicked: {
            cFile.reject()
        }
    }

    Menu {
        id: contextFileSendMenu
        property File file: null

        MenuItem {
            onTriggered: {
                contextFileSendMenu.file.cancel()
            }

            enabled : (contextFileSendMenu.file && contextFileSendMenu.file.state === File.FS_WAITING)
                || (contextFileSendMenu.file && contextFileSendMenu.file.state === File.FS_OFFERED)
                || (contextFileSendMenu.file && contextFileSendMenu.file.state === File.FS_TRANSFERRING)
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
            enabled: contextFileReceiveMenu.file && contextFileReceiveMenu.file.state == File.FS_OFFERED
        }

        MenuItem {
            onTriggered: {
                if (contextFileReceiveMenu.file && contextFileReceiveMenu.file.state == File.FS_TRANSFERRING) {
                    contextFileReceiveMenu.file.cancel();
                } else {
                    contextFileReceiveMenu.file.reject();
                }
            }

            text: (contextFileReceiveMenu.file && contextFileReceiveMenu.file.state == File.FS_TRANSFERRING)
                  ? qsTr("Cancel") : qsTr("Reject")
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
            enabled: contextFileReceiveMenu.file && contextFileReceiveMenu.file.state === File.FS_DONE
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
