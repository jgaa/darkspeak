import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Item {
    id: root
    width: cList.width
    height: card.implicitHeight + 12 * appearance.factor
    function showMenu() {
        const menu = cFile && cFile.direction === File.OUTGOING ? contextFileSendMenu : contextFileReceiveMenu;
        menu.file = cFile;
        menu.popup();
    }
    Pane {
        id: card
        width: Math.max(1, Math.min(root.width - 24 * appearance.factor, 680 * appearance.factor))
        x: cFile && cFile.direction === File.OUTGOING ? root.width - width - 8 * appearance.factor : 8 * appearance.factor
        padding: 10 * appearance.factor
        background: Rectangle {
            color: cFile && cFile.direction === File.OUTGOING ? appearance.outgoing : appearance.incoming
            radius: appearance.bubbleRadius
            border.color: appearance.border
        }
        contentItem: ColumnLayout {
            spacing: 6 * appearance.factor
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: cData.stateName || ""
                    textFormat: Text.PlainText
                    color: cFile && cFile.state === File.FS_FAILED ? appearance.error : appearance.secondaryText
                    Layout.fillWidth: true
                }
                Label {
                    text: cData.composedTime ? Qt.formatDateTime(cData.composedTime, Qt.DefaultLocaleShortDate) : ""
                    textFormat: Text.PlainText
                    color: appearance.secondaryText
                }
            }
            TextEdit {
                Layout.fillWidth: true
                text: cFile ? cFile.displayName : ""
                textFormat: TextEdit.PlainText
                font: appearance.font
                color: appearance.text
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
            }
            Label {
                text: cFile ? qsTr("%1 bytes").arg(Number(cFile.size).toLocaleString(Qt.locale(), 'f', 0)) : ""
                textFormat: Text.PlainText
                color: appearance.secondaryText
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: cFile ? cFile.progress : 0
                visible: cFile && (cFile.state === File.FS_QUEUED || cFile.state === File.FS_TRANSFERRING || cFile.state === File.FS_HASHING)
                Accessible.name: qsTr("File transfer progress")
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: qsTr("Accept and download")
                    visible: cFile && cFile.direction === File.INCOMING && cFile.state === File.FS_OFFERED
                    onClicked: cFile.accept()
                }
                Button {
                    text: qsTr("Reject")
                    visible: cFile && cFile.direction === File.INCOMING && cFile.state === File.FS_OFFERED
                    onClicked: cFile.reject()
                }
                Button {
                    text: qsTr("Resume")
                    visible: cFile && cFile.state === File.FS_FAILED
                    onClicked: cFile.resume()
                }
                Item {
                    Layout.fillWidth: true
                }
                ToolButton {
                    text: qsTr("Actions…")
                    enabled: !!cFile
                    onClicked: root.showMenu()
                }
            }
        }
    }
    Menu {
        id: contextFileSendMenu
        property File file: null

        MenuItem {
            onTriggered: {
                contextFileSendMenu.file.cancel();
            }

            enabled: (contextFileSendMenu.file && contextFileSendMenu.file.state === File.FS_WAITING) || (contextFileSendMenu.file && contextFileSendMenu.file.state === File.FS_OFFERED) || (contextFileSendMenu.file && contextFileSendMenu.file.state === File.FS_TRANSFERRING)
            text: qsTr("Cancel")
        }

        MenuItem {
            onTriggered: {
                manager.textToClipboard(contextFileSendMenu.file.name + ": " + contextFileSendMenu.file.hash);
            }

            text: qsTr("Copy Sha256")
        }

        MenuItem {
            onTriggered: {
                contextFileSendMenu.file.openInDefaultApplication();
            }
            text: qsTr("Open in external application")
        }

        MenuItem {
            onTriggered: {
                contextFileSendMenu.file.openFolder();
            }
            text: qsTr("Open exported copy’s folder")
        }
    }

    Menu {
        id: contextFileReceiveMenu
        property File file: null

        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.accept();
            }
            text: qsTr("Accept")
            enabled: contextFileReceiveMenu.file && contextFileReceiveMenu.file.state == File.FS_OFFERED
        }

        MenuItem {
            onTriggered: {
                if (contextFileReceiveMenu.file && contextFileReceiveMenu.file.state == File.FS_TRANSFERRING) {
                    contextFileReceiveMenu.file.cancel();
                } else {
                    contextFileReceiveMenu.file.reject();
                }
            }

            text: (contextFileReceiveMenu.file && contextFileReceiveMenu.file.state == File.FS_TRANSFERRING) ? qsTr("Cancel") : qsTr("Reject")
        }

        MenuItem {
            onTriggered: {
                manager.textToClipboard(contextFileReceiveMenu.file.name);
            }

            text: qsTr("Copy Name")
        }

        MenuItem {
            onTriggered: {
                manager.textToClipboard(contextFileReceiveMenu.file.name + ": " + contextFileReceiveMenu.file.hash);
            }

            text: qsTr("Copy Sha256")
        }

        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.openInDefaultApplication();
            }
            text: qsTr("Open in external application")
            enabled: contextFileReceiveMenu.file && contextFileReceiveMenu.file.state === File.FS_DONE
        }

        MenuItem {
            onTriggered: {
                contextFileReceiveMenu.file.openFolder();
            }
            text: qsTr("Open exported copy’s folder")
            enabled: contextFileReceiveMenu.file && contextFileReceiveMenu.file.state === File.FS_DONE
        }
    }
}
