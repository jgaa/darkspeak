import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Dialog {
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
    width: Math.min(Math.max(mainWindow.width - 80, 560), 800)
    height: Math.min(Math.max(mainWindow.height - 80, 420), 620)
    anchors.centerIn: parent
    padding: 16
    standardButtons: Dialog.Ok | Dialog.Cancel

    onOpened: {
        if (requestingIdentity && requesterIdentityId.length > 0)
            Qt.callLater(function() { requestingIdentity.probeContact(requesterIdentityId) })
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("Contact") }
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
                    spacing: 12

                    RoundedImage {
                        id: avatar
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 128
                        Layout.preferredHeight: 128
                        visible: source.length > 0
                        source: root.requesterAvatarUrl
                        border.width: 4
                        border.color: "gray"
                        color: "transparent"
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.requesterName || qsTr("Unknown contact")
                        color: "#243447"
                        font.bold: true
                        font.pixelSize: 18
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Message from contact:")
                        color: "#475569"
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
                    TextEdit {
                        width: root.width - 32
                        textFormat: TextEdit.MarkdownText
                        text: manager.sanitizeBio(root.probeBio)
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        onLinkActivated: function(link) {
                            manager.textToClipboard(link)
                            manager.setStatusMessage(qsTr("Bio link copied to clipboard"))
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
                manager.setTmpImageFromImage(root.requestingIdentity.probeAvatar)
                avatar.source = ""
                avatar.source = "image://temp/image"
            }
        }
    }
}
