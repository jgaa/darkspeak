
import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Dialogs 1.1

Item {
    id: root
    property Image avatar

    function open() {
        fileDialog.open()
    }

    FileDialog {
        id: fileDialog
        title: "Please select an avatar"
        folder: shortcuts.home

        onAccepted: {
            console.log("You chose avatar: " + fileDialog.fileUrl)
            avatar.source = darkRoot.prepareAvatar(fileDialog.fileUrl);
        }
        onRejected: {
            console.log("Canceled")
            close()
        }
    }
}
