import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Dialog {
    id: root
    property Conversation conversation: null
    standardButtons: StandardButton.Ok | StandardButton.Cancel
    width: mainWindow.width < 450 ? mainWindow.width : 450

    function updateInfo() {

    }

    ColumnLayout {
        spacing: 4
        width: parent.width

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 4
            Layout.fillWidth: parent.width
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Path")}
            Label { font.pointSize: 9; text: qsTr("Name")}
            Label { font.pointSize: 9; text: qsTr("Size")}
            Label { font.pointSize: 9; text: qsTr("Hash")}

            Row {
                Layout.fillWidth: true
                spacing: 4
                TextField {
                    id: path
                    width: parent.width - 50

                    onTextChanged: {
                        updateInfo()
                    }
                }

                Button {
                    x: path.right + 5
                    width: 45
                    text: "...";

                    onClicked: {
                        fileDialog.open()
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

            TextField {
                id: hash
                Layout.fillWidth: true
                readOnly: true
            }
        }

        FileDialog {
            id: fileDialog
            title: qsTr("File to Send")
            folder: shortcuts.home

            onAccepted: {
                path.text = fileDialog.fileUrl
            }
        }
    }

    onAccepted: {

//        const args = {
//            "identity" : identity.id,
//            "name" : name.text ? name.text : qsTr("Anononymous Coward"),
//            "nickName" : clip ? clip.nickName : null,
//            "handle" : handle.text,
//            "address" : address.text,
//            "addmeMessage" : addmeMessage.text,
//            "autoConnect" : autoConnect.checked,
//            "notes" : notes.text

//        }

        close()
        //destroy()
    }

    onRejected: {
        destroy();
    }
}
