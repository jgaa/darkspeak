import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com.jgaa.darkspeak 1.0

Page {
    id: root
    width: 600
    height: 400
    title: qsTr("Log")

    property bool follow: true

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
                color: level === 1 ? "#fff3b0" : "transparent"
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
                    color: level === 1 ? "#d32f2f"
                           : level === 2 ? "#f57c00"
                           : (level === 3 || level === 4) ? "#1976d2"
                           : level === 5 ? "#00897b"
                           : "#777777"
                }
            }
        }
    }
}
