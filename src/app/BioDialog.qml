import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Dialog {
    id: root
    property Contact contact: null
    title: contact ? qsTr("Bio — %1").arg(contact.name || contact.nickName) : qsTr("Bio")
    anchors.centerIn: parent
    width: Math.min(Math.max(mainWindow.width - 80, 480), 900)
    height: Math.min(Math.max(mainWindow.height - 80, 360), 760)
    standardButtons: Dialog.Close
    padding: 16

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        RoundedImage {
            visible: root.contact && root.contact.hasAvatar
            source: visible ? root.contact.avatarUrl : ""
            Layout.preferredWidth: 128
            Layout.preferredHeight: 128
            border.width: 4
            border.color: "gray"
            color: "transparent"
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            TextEdit {
                id: body
                width: root.width - 32
                textFormat: TextEdit.MarkdownText
                text: root.contact ? manager.sanitizeBio(root.contact.bio) : ""
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                onLinkActivated: function(link) {
                    manager.textToClipboard(link)
                    manager.setStatusMessage(qsTr("Bio link copied to clipboard"))
                }
            }
        }
    }
}
