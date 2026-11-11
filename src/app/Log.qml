import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Page {
    id: root
    width: 600 * appearance.factor
    height: 400 * appearance.factor
    title: qsTr("Log")

    property bool follow: true
    property color primaryText: appearance.text

    background: Rectangle {
        color: appearance.surface
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8 * appearance.factor
            anchors.rightMargin: 8 * appearance.factor

            Label {
                textFormat: Text.PlainText
                text: qsTr("Log")
                font.pixelSize: appearance.fontPixels * 1.4
                Layout.fillWidth: true
            }

            TextField {
                Layout.preferredWidth: 220 * appearance.factor
                placeholderText: qsTr("Filter log messages")
                Accessible.name: qsTr("Filter log messages")
                onTextEdited: filteredLog.search = text
            }
            ComboBox {
                model: [qsTr("Errors"), qsTr("Warnings"), qsTr("Notices"), qsTr("Info"), qsTr("Debug"), qsTr("Trace")]
                currentIndex: filteredLog.maximumLevel - 1
                onActivated: filteredLog.maximumLevel = currentIndex + 1
                Accessible.name: qsTr("Maximum log verbosity")
            }
            ToolButton {
                text: qsTr("Follow")
                checkable: true
                checked: root.follow
                onToggled: root.follow = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Keep the latest message visible")
            }
        }
    }

    ListView {
        id: listView
        model: filteredLog
        clip: true
        anchors.fill: parent
        spacing: 0
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        function scrollToLatest() {
            if (root.follow) {
                Qt.callLater(function () {
                    listView.positionViewAtEnd();
                });
            }
        }

        onCountChanged: scrollToLatest()

        delegate: Item {
            height: logText.height + 3
            width: listView.width
            Rectangle {
                anchors.fill: parent
                color: level === 1 ? appearance.surface : level === 2 ? appearance.surface : (level === 3 || level === 4) ? appearance.surface : level === 5 ? appearance.surface : (index % 2 === 0 ? appearance.surface : appearance.sidebar)
            }
            Row {
                id: row1
                TextEdit {
                    id: logText
                    anchors.margins: 3 * appearance.factor
                    text: display
                    width: root.width - 10
                    readOnly: true
                    selectByMouse: true
                    persistentSelection: true
                    renderType: Text.QtRendering
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    anchors.verticalCenter: parent.verticalCenter
                    font: appearance.captionFont
                    color: level === 1 ? appearance.error : level === 2 ? appearance.warning : (level === 3 || level === 4) ? appearance.accent : level === 5 ? appearance.success : root.primaryText
                }
            }
        }
    }
}
