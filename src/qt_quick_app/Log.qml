import QtQuick 2.12
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Page {
    id: root
    width: 600
    height: 400
    title: qsTr("Log")

    header: Label {
        id: label
        text: qsTr("Log")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }

    ListView {
        id: listView
        model: log
        clip: true
        anchors.fill: parent
        delegate: Item {
            height: logText.height + 3
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
                }
            }
        }
    }
}
