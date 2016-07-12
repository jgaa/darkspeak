
import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Dialogs 1.1
import com.jgaa.darkspeak 1.0

Item {
    id: root
    property string fileName
    property string fileId
    property ContactData contact

    function process() {

        console.log("Querying regarding file \"" + fileName
            + "\" with id " + fileId)

        incomingFileResponse.text = contact.profileName
            + " [" + contact.handle
            + "] wants to send you a file with name \""
            + fileName
            + "\". Do you want to accept this file?"

        incomingFileResponse.open()
    }

    MessageDialog {
        id: incomingFileResponse
        title: "Incoming File Request"
        icon: StandardIcon.Warning

        standardButtons: StandardButton.Yes | StandardButton.Cancel
        onYes: {
            console.log("Accepting file " + fileId)
            darkRoot.acceptFile(contact.handle, fileId)
        }
        onRejected: {
            console.log("Discarding file " + fileId)
            darkRoot.rejectFile(contact.handle, fileId)
        }
    }
}
