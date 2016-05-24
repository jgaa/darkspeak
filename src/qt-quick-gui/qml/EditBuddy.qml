
import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0
import QtQuick.Layouts 1.2

Dialog {
    id: root
    title: "Edit Contact"
    standardButtons: StandardButton.Ok | StandardButton.Cancel
    width: main_window.width

    //height: 100
    property ContactData buddy

    GridLayout {
        columns: 2
        width: root.width

        Text { text: "Id" } DataTextRo { text: buddy.handle; Layout.fillWidth: true }
        Text { text: "Name" } DataTextRo { text: buddy.profileName; Layout.fillWidth: true }
        Text { text: "Info" } DataTextRo { text: buddy.profileText; Layout.fillWidth: true;
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                height: 90
        }
        Text { text: "Nickname" } DataText { text: buddy.ourNickname; Layout.fillWidth: true;}
        Text { text: "Notes" } DataText { text: buddy.ourGeneralNotes;
                Layout.fillWidth: true; wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                height: 90
        }
        Text { text: "Created" } DataTextRo { text: buddy.createdTime;Layout.fillWidth: true; }
        Text { text: "First contact" } DataTextRo { text: buddy.firstContact;Layout.fillWidth: true; }
        //Text { text: "Last seen" } DataTextRo { text: buddy.createdTime }

    }

    onButtonClicked: {
        if (clickedButton === StandardButton.Ok) {
            console.log("Saving buddy")
            buddy.save()

        } else {
            console.log("Cancelled" + clickedButton)
        }
    }
}

