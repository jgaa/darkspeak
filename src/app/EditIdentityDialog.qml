import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

UiDialog {
    id: root
    property Identity identity: null
    property QmlIdentityReq value: QmlIdentityReq {}

    property bool mimeTypesValid: value.validMimeText(mimeTypes.text)
    standardButtons: name.acceptableInput && mimeTypesValid && (identity || manager.online) ? (Dialog.Ok | Dialog.Cancel) : Dialog.Cancel
    title: identity ? qsTr("Edit Identity") : qsTr("Create a new Identity")
    width: Math.min(mainWindow.width - 24 * appearance.factor, 1000 * appearance.factor)
    height: Math.min(mainWindow.height - 24 * appearance.factor, 800 * appearance.factor)

    Component.onCompleted: {
        if (identity) {
            value.name = identity.name;
            value.notes = identity.notes;
            value.bio = identity.bio;
            value.autoConnect = identity.autoConnect;
            value.avatar = identity.avatar;
            value.previewImages = identity.previewImages;
            value.previewVideo = identity.previewVideo;
            value.allowUtf8 = identity.allowUtf8;
            value.allowFiles = identity.allowFiles;
            value.allowMimeTypes = identity.allowMimeTypes;
            value.allowReactions = identity.allowReactions;
            value.publicBio = identity.publicBio;
            value.publicAvatar = identity.publicAvatar;
        }
        mimeTypes.text = value.allowMimeTypes.join("\n");
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10 * appearance.factor

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton {
                text: qsTr("Profile")
            }
            TabButton {
                text: qsTr("Bio")
            }
            TabButton {
                text: qsTr("Security preferences")
            }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: appearance.spacing

                    GridLayout {
                        rowSpacing: appearance.spacing
                        rows: 2
                        flow: GridLayout.TopToBottom
                        Layout.fillWidth: true

                        Label {
                            textFormat: Text.PlainText
                            text: qsTr("Name")
                        }
                        Label {
                            textFormat: Text.PlainText
                            text: qsTr("Avatar")
                        }

                        TextField {
                            id: name
                            Layout.fillWidth: true
                            placeholderText: qsTr("Anonymous Coward")
                            text: value.name
                            validator: IdentityNameValidator {
                                currentName: identity ? identity.name : null
                            }
                            color: acceptableInput ? appearance.text : appearance.error
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Image {
                                id: avatar
                                Layout.preferredWidth: 96 * appearance.factor
                                Layout.preferredHeight: 96 * appearance.factor
                                fillMode: Image.PreserveAspectFit
                                source: identity ? identity.avatarUrl : "image://temp/image"
                                cache: false
                            }
                            Button {
                                text: qsTr("Select ...")
                                onClicked: {
                                    const selected = fileDialogs.openFile(qsTr("Avatar Image"), "", [qsTr("Image files (*.jpg *.jpeg *.png)")]);
                                    if (selected) {
                                        manager.setTmpImageFromPath(selected);
                                        avatar.source = "";
                                        avatar.source = "image://temp/image";
                                        value.avatar = manager.getTmpImage();
                                    }
                                }
                            }
                        }
                    }

                    CheckBox {
                        id: autoConnect
                        text: qsTr("Auto Connect")
                        checked: value.autoConnect
                    }

                    Label {
                        textFormat: Text.PlainText
                        visible: !identity && !manager.online
                        text: qsTr("Connect to Tor before creating an identity.")
                        color: appearance.error
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Notes")
                    }
                    TextArea {
                        id: notes
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: value.notes
                    }
                }
            }

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: appearance.spacing

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            textFormat: Text.PlainText
                            text: qsTr("Bio")
                            font.bold: true
                            Layout.fillWidth: true
                        }
                        Label {
                            textFormat: Text.PlainText
                            text: qsTr("Markdown is supported")
                            color: appearance.secondaryText
                        }
                        Label {
                            textFormat: Text.PlainText
                            text: qsTr("%1 remaining").arg(Math.max(0, 1000 - bioEdit.text.length))
                            color: bioEdit.text.length >= 1000 ? appearance.error : appearance.success
                        }
                    }

                    TabBar {
                        id: bioTabs
                        Layout.fillWidth: true
                        TabButton {
                            text: qsTr("Edit")
                        }
                        TabButton {
                            text: qsTr("Preview")
                        }
                    }

                    StackLayout {
                        currentIndex: bioTabs.currentIndex
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        TextArea {
                            id: bioEdit
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: value.bio
                            wrapMode: TextEdit.Wrap
                            onTextChanged: {
                                if (text.length > 1000) {
                                    const position = cursorPosition;
                                    text = text.slice(0, 1000);
                                    cursorPosition = Math.min(position, 1000);
                                }
                            }
                        }

                        ScrollView {
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                            TextEdit {
                                width: parent.width
                                textFormat: TextEdit.MarkdownText
                                text: manager.sanitizeBio(bioEdit.text)
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

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10 * appearance.factor

                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("These preferences control what contacts may send or learn. Local image and video previews are disabled by default.")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    CheckBox {
                        id: previewImages
                        text: qsTr("Preview received images locally")
                        checked: value.previewImages
                    }
                    CheckBox {
                        id: previewVideo
                        text: qsTr("Preview received video locally")
                        checked: value.previewVideo
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Preview preferences do not accept or download files. Avatars use the separate restricted bitmap pipeline.")
                        wrapMode: Text.WordWrap
                    }
                    CheckBox {
                        id: allowUtf8
                        text: qsTr("Allow UTF-8 text")
                        checked: value.allowUtf8
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("When UTF-8 is disabled, incoming non-ASCII text is refused and older non-ASCII text is hidden. Interface translations are unaffected.")
                        wrapMode: Text.WordWrap
                        color: appearance.secondaryText
                    }
                    CheckBox {
                        id: allowFiles
                        text: qsTr("Allow files")
                        checked: value.allowFiles
                    }
                    CheckBox {
                        id: allowReactions
                        text: qsTr("Allow reactions")
                        checked: value.allowReactions
                    }
                    CheckBox {
                        id: publicBio
                        text: qsTr("Allow public Bio visibility")
                        checked: value.publicBio
                    }
                    CheckBox {
                        id: publicAvatar
                        text: qsTr("Allow public avatar visibility")
                        checked: value.publicAvatar
                    }

                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Accepted MIME types (one per line; use * for all)")
                        Layout.fillWidth: true
                    }
                    TextArea {
                        id: mimeTypes
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        placeholderText: "*"
                        wrapMode: TextEdit.NoWrap
                        color: mimeTypesValid ? appearance.text : appearance.error
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: mimeTypesValid ? "" : qsTr("Enter valid lowercase MIME types, one per line. Use only * by itself for all types.")
                        color: appearance.error
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    onAccepted: {
        value.name = name.text ? name.text : qsTr("Anonymous Coward");
        value.notes = notes.text;
        value.bio = bioEdit.text;
        value.autoConnect = autoConnect.checked;
        value.previewImages = previewImages.checked;
        value.previewVideo = previewVideo.checked;
        value.allowUtf8 = allowUtf8.checked;
        value.allowFiles = allowFiles.checked;
        value.allowReactions = allowReactions.checked;
        value.publicBio = publicBio.checked;
        value.publicAvatar = publicAvatar.checked;
        if (!value.setMimeText(mimeTypes.text))
            return;
        if (identity) {
            identity.name = value.name;
            identity.notes = value.notes;
            identity.bio = value.bio;
            identity.autoConnect = value.autoConnect;
            identity.avatar = value.avatar;
            identity.previewImages = value.previewImages;
            identity.previewVideo = value.previewVideo;
            identity.allowUtf8 = value.allowUtf8;
            identity.allowFiles = value.allowFiles;
            identity.allowReactions = value.allowReactions;
            identity.publicBio = value.publicBio;
            identity.publicAvatar = value.publicAvatar;
            identity.allowMimeTypes = value.allowMimeTypes;
        } else {
            identities.createIdentity(value);
        }
        destroy();
    }

    onRejected: destroy()
}
