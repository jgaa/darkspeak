import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

Page {
    id: root
    width: 600
    height: 400
    title: qsTr("Log")

    property bool follow: true
    property color primaryText: "#334155"

    background: Rectangle {
        color: "#f7f9fc"
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8

            Label {
                text: qsTr("Log")
                font.pixelSize: Qt.application.font.pixelSize * 1.4
                Layout.fillWidth: true
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
        model: log
        clip: true
        anchors.fill: parent
        spacing: 0
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        function scrollToLatest() {
            if (root.follow) {
                Qt.callLater(function() { listView.positionViewAtEnd() })
            }
        }

        onCountChanged: scrollToLatest()

        delegate: Item {
            height: logText.height + 3
            width: listView.width
            Rectangle {
                anchors.fill: parent
                color: level === 1 ? "#fff1f0"
                       : level === 2 ? "#fff7e6"
                       : (level === 3 || level === 4) ? "#eef6ff"
                       : level === 5 ? "#edf9f7"
                       : (index % 2 === 0 ? "#f7f9fc" : "#eef2f7")
            }
            Row {
                id: row1
                Text {
                    id: logText
                    anchors.margins: 3
                    text: display
                    width: root.width - 10
                    renderType: Text.QtRendering
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    anchors.verticalCenter: parent.verticalCenter
                    font.pointSize: 8
                    color: level === 1 ? "#b42318"
                           : level === 2 ? "#9a6700"
                           : (level === 3 || level === 4) ? "#175cd3"
                           : level === 5 ? "#087f5b"
                           : root.primaryText
                }
            }
        }
    }
}
