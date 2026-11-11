import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

UiDialog {
    id: root
    property Conversation conversation: null
    title: qsTr("Offer a file")
    width: Math.min(mainWindow.width - 24 * appearance.factor, 600 * appearance.factor)
    standardButtons: path.text.length > 0 && conversation && conversation.participant && conversation.participant.canSendFiles ? Dialog.Ok | Dialog.Cancel : Dialog.Cancel
    function updateInfo(filePath) {
        name.text = files.getFileName(filePath);
    }
    contentItem: ColumnLayout {
        spacing: 10 * appearance.factor
        Label {
            text: qsTr("File")
        }
        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: path
                Layout.fillWidth: true
                Accessible.name: qsTr("File path")
                onEditingFinished: root.updateInfo(text)
            }
            Button {
                text: qsTr("Browse…")
                onClicked: {
                    const selected = fileDialogs.openFile(qsTr("File to send"), path.text);
                    if (selected) {
                        path.text = selected;
                        root.updateInfo(selected);
                    }
                }
            }
        }
        Label {
            text: qsTr("Display filename")
        }
        TextField {
            id: name
            Layout.fillWidth: true
            Accessible.name: qsTr("Display filename")
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("The contact's current file and MIME preferences are checked before an offer is queued.")
            wrapMode: Text.WordWrap
            color: appearance.secondaryText
        }
    }
    onAccepted: {
        conversation.sendFile({
            name: name.text || files.getFileName(path.text),
            path: path.text
        });
        destroy();
    }
    onRejected: destroy()
}
