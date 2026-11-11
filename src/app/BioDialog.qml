import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

UiDialog {
    id: root
    property Contact contact: null
    title: contact ? qsTr("Bio — %1").arg(contact.name || contact.nickName) : qsTr("Bio")
    anchors.centerIn: parent
    width: Math.min(mainWindow.width - 24 * appearance.factor, 900 * appearance.factor)
    height: Math.min(mainWindow.height - 24 * appearance.factor, 760 * appearance.factor)
    standardButtons: Dialog.Close
    onClosed: destroy()
    padding: appearance.largeSpacing

    ColumnLayout {
        anchors.fill: parent
        spacing: 10 * appearance.factor

        RoundedImage {
            visible: root.contact && root.contact.hasAvatar
            source: visible ? root.contact.avatarUrl : ""
            Layout.preferredWidth: 128 * appearance.factor
            Layout.preferredHeight: 128 * appearance.factor
            border.width: 4 * appearance.factor
            border.color: appearance.secondaryText
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
                onLinkActivated: function (link) {
                    manager.textToClipboard(link);
                    manager.setStatusMessage(qsTr("Bio link copied to clipboard"));
                }
            }
        }
    }
}
