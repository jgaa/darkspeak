
import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Dialogs 1.1

Item {
    id: root
    property string buddyId
    property string buddyName

    function open() {
        fileDialog.open()
    }

    FileDialog {
        id: fileDialog
        title: "Please select a file"
        folder: shortcuts.home

        onAccepted: {
            console.log("You chose: " + fileDialog.fileUrls)

            confirmation.text = "Do you really want to send the file '"
                + fileDialog.fileUrls
                + "' to " + buddyName + "?"

            confirmation.open()
        }
        onRejected: {
            console.log("Canceled")
            close()
        }
    }

    MessageDialog {
        id: confirmation
        title: "Please Confirm"
        icon: StandardIcon.Warning

        standardButtons: StandardButton.Yes | StandardButton.Cancel
        onYes: {
            console.log("Sending file " + fileDialog.fileUrls)
            transfersModel.sendFile(buddyId, fileDialog.fileUrl)
        }
        onRejected: {
            console.log("Discarding file " + fileDialog.fileUrls)
        }
    }
}
