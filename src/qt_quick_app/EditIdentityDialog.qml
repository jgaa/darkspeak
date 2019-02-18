import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Dialogs 1.2
import QtQuick.Layouts 1.3
import com.jgaa.darkspeak 1.0

Dialog {
    id: root
    property Identity identity: null
    property QmlIdentityReq value: QmlIdentityReq {}
    standardButtons: StandardButton.Ok | StandardButton.Cancel

    Component.onCompleted: {
        title = identity ? qsTr("Edit identity") : qsTr("Create a new identity")

        // Initialize from Identity if we have one
        if (identity) {
            value.name = identity.name;
            value.notes = identity.notes;
            value.autoConnect = identity.autoConnect;
        }
    }

    ColumnLayout {
        spacing: 4
        Layout.fillWidth: parent.width

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 1
            Layout.fillWidth: parent.width
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Name")}

            TextField {
                id: name
                Layout.fillWidth: true
                placeholderText: qsTr("Anonymous Coward")
                text: value.name
            }
        }

        RowLayout {
            spacing: 4
            CheckBox {
                id: autoConnect
                text: qsTr("Auto Connect")
                checked: value.autoConnect
            }
        }

        Label { font.pointSize: 9; text: qsTr("Notes")}

        TextArea {
            id: notes
            Layout.fillHeight: true
            Layout.minimumHeight: 30
            Layout.minimumWidth: 3
            Layout.fillWidth: true
            text: value.notes
        }
    }

    onAccepted: {
        value.name = name.text ? name.text : qsTr("Anonymous Coward")
        value.notes = notes.text
        value.autoConnect = autoConnect.checked

        if (identity) {
            identity.name = value.name;
            identity.notes = value.notes;
            identity.autoConnect = value.autoConnect;
        } else {
            // Add a new identity
            identities.createIdentity(value)
        }
        close()
    }

    onRejected: {
        close();
    }
}





































/*##^## Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
 ##^##*/
