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
    property int dlg_width: mainWindow.width > 400 ? 400 : mainWindow.width
    property int dlg_height: mainWindow.height > 500 ? 370 : mainWindow.height
    width: dlg_width
    height: dlg_height

    Component.onCompleted: {
        root.title = identity ? qsTr("Edit Identity") : qsTr("Create a new Identity")

        // Initialize from Identity if we have one
        if (identity) {
            value.name = identity.name;
            value.notes = identity.notes;
            value.autoConnect = identity.autoConnect;
            value.avatar = identity.avatar
        }
    }

    ColumnLayout {
        spacing: 4
        anchors.fill: parent

        GridLayout {
            id: fields
            rowSpacing: 4
            rows: 2
            Layout.fillWidth: parent.width
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Name")}
            Label { font.pointSize: 9; text: qsTr("Avatar")}

            TextField {
                id: name
                Layout.fillWidth: true
                placeholderText: qsTr("Anonymous Coward")
                text: value.name
            }

            Row {
                id: row
                height: avatar.height
                Layout.fillWidth: true
                spacing: 6

                Image {
                    id: avatar
                    width: 64
                    height: 64
                    Layout.maximumHeight: 128
                    Layout.maximumWidth: 128
                    Layout.minimumHeight: 32
                    Layout.minimumWidth: 32
                    Layout.preferredHeight: 64
                    Layout.preferredWidth: 64
                    fillMode: Image.PreserveAspectFit
                    source: identity ? identity.avatarUrl : "image://temp/image";
                    cache: false
                }

                Button {
                    id: avatarBtn
                    text: qsTr("Select ...");
                    anchors.verticalCenter: avatar.verticalCenter
                    anchors.left: avatar.right
                    anchors.leftMargin: 6
                    onClicked: {
                        avatarSelector.open()
                    }
                }

                FileDialog {
                    id: avatarSelector
                    title: qsTr("Avatar Image")
                    selectExisting: true
                    selectMultiple: false

                    nameFilters: ["Image files (*.jpg, *.jpeg, *.png)"]
                    onAccepted: {
                        var url = fileUrl
                        manager.setTmpImageFromPath(url);

                        // Give it a unique url so it fetches the new image...
                        //var newUrl = "image://temp/image" + Math.floor(Math.random() * 100000000);
                        //avatar.source = newUrl;
                        avatar.source = "";
                        avatar.source = "image://temp/image";
                        value.avatar = manager.getTmpImage()
                    }
                }
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
            identity.name = value.name
            identity.notes = value.notes
            identity.autoConnect = value.autoConnect
            identity.avatar = value.avatar
        } else {
            // Add a new identity
            identities.createIdentity(value)
        }
        destroy()
    }

    onRejected: {
        destroy();
    }
}





/*##^## Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
 ##^##*/
