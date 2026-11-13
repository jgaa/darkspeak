import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

UiDialog {
    id: root
    required property var support
    objectName: "videoPreviewDependencyDialog"
    title: qsTr("Video previews need FFmpeg")
    width: Math.min(560 * appearance.factor, parent ? parent.width - 24 * appearance.factor : 560)
    height: Math.min(380 * appearance.factor, parent ? parent.height - 24 * appearance.factor : 380)
    standardButtons: Dialog.Ok
    onClosed: support.dismiss(dontShowAgain.checked)
    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: appearance.spacing
            Label {
                objectName: "ffmpegInstallAdvice"
                Layout.fillWidth: true
                text: qsTr("FFmpeg was not found. Install it if you want DarkSpeak to generate video previews. You can continue using DarkSpeak without it.")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                text: root.support.installCommand.length > 0
                    ? qsTr("Install it using your distribution’s package manager:")
                    : qsTr("For video previews, follow the instructions for installing FFmpeg on your platform.")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }
            TextArea {
                objectName: "ffmpegInstallCommand"
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.support.installCommand
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
            }
            CheckBox {
                id: dontShowAgain
                objectName: "dontShowFfmpegWarningAgain"
                text: qsTr("Don’t show again")
            }
        }
    }
}
