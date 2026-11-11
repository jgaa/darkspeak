import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

UiDialog {
    id: root

    property int notificationRow: -1
    property Identity requestingIdentity: null
    property string requesterName: ""
    property string requestMessage: ""
    property string requesterAvatarUrl: ""
    property string requesterIdentityId: ""
    property string probeBio: requestingIdentity ? requestingIdentity.probeBio : ""
    property bool hasBio: probeBio.length > 0

    title: qsTr("Accept Contact")
    width: Math.min(mainWindow.width - 24 * appearance.factor, 800 * appearance.factor)
    height: Math.min(mainWindow.height - 24 * appearance.factor, 620 * appearance.factor)
    anchors.centerIn: parent
    padding: appearance.largeSpacing
    standardButtons: Dialog.Ok | Dialog.Cancel

    onOpened: {
        if (requestingIdentity && requesterIdentityId.length > 0)
            Qt.callLater(function () {
                requestingIdentity.probeContact(requesterIdentityId);
            });
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6 * appearance.factor

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton {
                text: qsTr("Contact")
            }
            TabButton {
                text: qsTr("Bio")
                enabled: root.hasBio
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12 * appearance.factor

                    RoundedImage {
                        id: avatar
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 128 * appearance.factor
                        Layout.preferredHeight: 128 * appearance.factor
                        visible: source.length > 0
                        source: root.requesterAvatarUrl
                        border.width: 4 * appearance.factor
                        border.color: appearance.secondaryText
                        color: "transparent"
                    }

                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: root.requesterName || qsTr("Unknown contact")
                        color: appearance.text
                        font: appearance.headingFont
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Message from contact:")
                        color: appearance.secondaryText
                    }

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        TextArea {
                            width: root.width - 48
                            text: root.requestMessage
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextArea.Wrap
                        }
                    }
                }
            }

            Item {
                ScrollView {
                    anchors.fill: parent
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    TextEdit {
                        width: root.width - 32
                        textFormat: TextEdit.MarkdownText
                        text: manager.sanitizeBio(root.probeBio)
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
    }

    Connections {
        target: root.requestingIdentity
        function onProbeAvatarChanged() {
            if (root.requestingIdentity && root.requestingIdentity.hasProbeAvatar) {
                manager.setTmpImageFromImage(root.requestingIdentity.probeAvatar);
                avatar.source = "";
                avatar.source = "image://temp/image";
            }
        }
    }
}
