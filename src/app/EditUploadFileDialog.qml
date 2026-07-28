import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Dialog {
    id: root
    property Conversation conversation: null
    modal: true
    closePolicy: Popup.NoAutoClose
    standardButtons: path.text ? (Dialog.Ok | Dialog.Cancel) : Dialog.Cancel
    width: mainWindow.width < 450 ? mainWindow.width : 450

    function updateInfo(fpath) {
        name.text = files.getFileName(fpath)
        var len = files.getFileLength(fpath)
        size.text = len >= 0 ? len.toString() : ""
    }

    ColumnLayout {
        spacing: 4
        width: parent.width

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 3
            Layout.fillWidth: parent.width
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Path")}
            Label { font.pointSize: 9; text: qsTr("Name")}
            Label { font.pointSize: 9; text: qsTr("Size")}

            Row {
                Layout.fillWidth: true
                spacing: 4
                TextField {
                    id: path
                    width: parent.width - 50

                    onTextChanged: {
                        onTextChanged: console.log("Text has changed to:", text)
                        updateInfo(text)
                    }
                }

                Button {
                    x: path.right + 5
                    width: 45
                    text: "...";

                    onClicked: {
                        const selected = fileDialogs.openFile(
                            qsTr("File to Send"), path.text)
                        if (selected)
                            path.text = selected
                    }
                }
            }

            TextField {
                id: name
                Layout.fillWidth: true
            }

            TextField {
                id: size
                width: 200
                readOnly: true
            }
        }

    }

    onAccepted: {
        const args = {
            "name" : name.text ? name.text : files.getFileName(path.text),
            "path" : path.text
        }

        conversation.sendFile(args);

        //close()
        destroy()
    }

    onRejected: {
        destroy();
    }
}
