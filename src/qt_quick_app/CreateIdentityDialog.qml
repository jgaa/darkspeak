import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Dialog {
    id: customDialog
    title: qsTr("Create a new identify")
    standardButtons: StandardButton.Ok | StandardButton.Cancel

    Column {
        anchors.fill: parent
        Text {
            id: topText
            color: "#2a1e1e"
            text:  qsTr("What is your unique avatar name?")
        }

        Row {
            id: rowId
            y: topText.bottom
            anchors.margins: 16
            Text {
                id: textLabel
                text: "Avatar: "
            }

            TextField {
                anchors.margins: 16
                id: avatar
                text:  qsTr("Anonymous Coward")
               // anchors.right: parent.right
                anchors.left: textLabel.right + 10
                focus: true
            }
        }

        Text{
            anchors.margins: 16
            text: qsTr("Please note that you must be connected to the Tor network
in order to create an identity.
It may take a few seconds from you press OK
until the identity becomes visible.")
        }
    }

    onVisibleChanged: {
        if (visible === true) {
            avatar.selectAll()
        }
    }

    onButtonClicked: {
        if (clickedButton===StandardButton.Ok) {
            console.log("Accepted " + clickedButton)
            identities.createIdentity(avatar.text)
        } else {
            console.log("Rejected" + clickedButton)
        }
    }
}
















































/*##^## Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
 ##^##*/
