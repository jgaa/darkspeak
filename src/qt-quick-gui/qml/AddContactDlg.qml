import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Dialogs 1.2

Dialog {
    title: "Add Contact"
    standardButtons: StandardButton.Ok | StandardButton.Cancel
    width: main_window.width
    height: 100

    Column {
        anchors.fill: parent

        Rectangle {
            anchors.fill: edId
            color: "light gray"
        }

        TextField {
            id: edId
            width: parent.width
            placeholderText: "<Tor Chat ID>"
        }
    }

    onButtonClicked: {
        if (clickedButton === StandardButton.Ok) {
            console.log("Adding " + clickedButton)
            contacts.model.append(edId.text)

        } else {
            console.log("Cancelled" + clickedButton)
        }
    }
}

