import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Dialog {
    id: root
    property Identity identity: null
    property QmlIdentityReq value: QmlIdentityReq {}

    property bool mimeTypesValid: validateMimeTypes(mimeTypes.text)
    standardButtons: name.acceptableInput && mimeTypesValid && (identity || manager.online)
                     ? (Dialog.Ok | Dialog.Cancel)
                     : Dialog.Cancel
    title: identity ? qsTr("Edit Identity") : qsTr("Create a new Identity")
    width: Math.min(Math.max(mainWindow.width - 80, 700), 1000)
    height: Math.min(Math.max(mainWindow.height - 80, 600), 800)

    function validateMimeTypes(text) {
        const lines = text.split(/\r?\n/).map(function(line) { return line.trim() })
        const nonEmpty = lines.filter(function(line) { return line.length > 0 })
        if (nonEmpty.length > 64) return false

        const wildcard = nonEmpty.indexOf("*") >= 0
        if (wildcard) return nonEmpty.length === 1

        const mimePattern = /^[a-z0-9!#$&^_.+\-]+\/[a-z0-9!#$&^_.+\-]+$/
        const seen = {}
        for (let i = 0; i < nonEmpty.length; ++i) {
            const type = nonEmpty[i]
            if (type.length > 127 || !mimePattern.test(type) || seen[type]) return false
            seen[type] = true
        }
        return nonEmpty.length > 0
    }

    Component.onCompleted: {
        if (identity) {
            value.name = identity.name
            value.notes = identity.notes
            value.bio = identity.bio
            value.autoConnect = identity.autoConnect
            value.avatar = identity.avatar
            value.allowUtf8 = identity.allowUtf8
            value.allowFiles = identity.allowFiles
            value.allowMimeTypes = identity.allowMimeTypes
            value.allowReactions = identity.allowReactions
            value.publicBio = identity.publicBio
            value.publicAvatar = identity.publicAvatar
        }
        mimeTypes.text = value.allowMimeTypes.join("\n")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("Profile") }
            TabButton { text: qsTr("Bio") }
            TabButton { text: qsTr("Security preferences") }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    GridLayout {
                        rowSpacing: 8
                        rows: 2
                        flow: GridLayout.TopToBottom
                        Layout.fillWidth: true

                        Label { text: qsTr("Name") }
                        Label { text: qsTr("Avatar") }

                        TextField {
                            id: name
                            Layout.fillWidth: true
                            placeholderText: qsTr("Anonymous Coward")
                            text: value.name
                            validator: IdentityNameValidator {
                                currentName: identity ? identity.name : null
                            }
                            color: acceptableInput ? "black" : "red"
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Image {
                                id: avatar
                                Layout.preferredWidth: 96
                                Layout.preferredHeight: 96
                                fillMode: Image.PreserveAspectFit
                                source: identity ? identity.avatarUrl : "image://temp/image"
                                cache: false
                            }
                            Button {
                                text: qsTr("Select ...")
                                onClicked: {
                                    const selected = fileDialogs.openFile(
                                        qsTr("Avatar Image"), "",
                                        [qsTr("Image files (*.jpg *.jpeg *.png)")])
                                    if (selected) {
                                        manager.setTmpImageFromPath(selected)
                                        avatar.source = ""
                                        avatar.source = "image://temp/image"
                                        value.avatar = manager.getTmpImage()
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
                        visible: !identity && !manager.online
                        text: qsTr("Connect to Tor before creating an identity.")
                        color: "firebrick"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Label { text: qsTr("Notes") }
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
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Bio")
                            font.bold: true
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("Markdown is supported")
                            color: "#334e68"
                        }
                        Label {
                            text: qsTr("%1 remaining").arg(Math.max(0, 1000 - bioEdit.text.length))
                            color: bioEdit.text.length >= 1000 ? "#b91c1c" : "#166534"
                        }
                    }

                    TabBar {
                        id: bioTabs
                        Layout.fillWidth: true
                        TabButton { text: qsTr("Edit") }
                        TabButton { text: qsTr("Preview") }
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
                                    const position = cursorPosition
                                    text = text.slice(0, 1000)
                                    cursorPosition = Math.min(position, 1000)
                                }
                            }
                        }

                        ScrollView {
                            clip: true
                            TextEdit {
                                width: parent.width
                                textFormat: TextEdit.MarkdownText
                                text: manager.sanitizeBio(bioEdit.text)
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

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    Label {
                        text: qsTr("These preferences control what your contacts may send or learn. Everything is allowed by default.")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    CheckBox { id: allowUtf8; text: qsTr("Allow UTF-8 text"); checked: value.allowUtf8 }
                    CheckBox { id: allowFiles; text: qsTr("Allow files"); checked: value.allowFiles }
                    CheckBox { id: allowReactions; text: qsTr("Allow reactions"); checked: value.allowReactions }
                    CheckBox { id: publicBio; text: qsTr("Allow public Bio visibility"); checked: value.publicBio }
                    CheckBox { id: publicAvatar; text: qsTr("Allow public avatar visibility"); checked: value.publicAvatar }

                    Label {
                        text: qsTr("Accepted MIME types (one per line; use * for all)")
                        Layout.fillWidth: true
                    }
                    TextArea {
                        id: mimeTypes
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        placeholderText: "*"
                        wrapMode: TextEdit.NoWrap
                        color: mimeTypesValid ? "black" : "firebrick"
                    }
                    Label {
                        text: mimeTypesValid ? "" : qsTr("Enter valid lowercase MIME types, one per line. Use only * by itself for all types.")
                        color: "firebrick"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    onAccepted: {
        value.name = name.text ? name.text : qsTr("Anonymous Coward")
        value.notes = notes.text
        value.bio = bioEdit.text
        value.autoConnect = autoConnect.checked
        value.allowUtf8 = allowUtf8.checked
        value.allowFiles = allowFiles.checked
        value.allowReactions = allowReactions.checked
        value.publicBio = publicBio.checked
        value.publicAvatar = publicAvatar.checked
        value.allowMimeTypes = mimeTypes.text.split(/\r?\n/).map(function(line) {
            return line.trim()
        }).filter(function(line) { return line.length > 0 })

        if (identity) {
            identity.name = value.name
            identity.notes = value.notes
            identity.bio = value.bio
            identity.autoConnect = value.autoConnect
            identity.avatar = value.avatar
            identity.allowUtf8 = value.allowUtf8
            identity.allowFiles = value.allowFiles
            identity.allowReactions = value.allowReactions
            identity.publicBio = value.publicBio
            identity.publicAvatar = value.publicAvatar
            identity.allowMimeTypes = value.allowMimeTypes
        } else {
            identities.createIdentity(value)
        }
        destroy()
    }

    onRejected: destroy()
}
