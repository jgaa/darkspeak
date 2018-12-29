import QtQuick 2.12
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Page {
    id: page
    width: 600
    height: 400
    title: qsTr("Log")

    header: Label {
        text: qsTr("Log")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }

    ListView {
        id: listView
        model: log
        anchors.fill: parent
        delegate: Item {
            height: logText.height + 2
            Row {
                id: row1
                Text {
                    id: logText
                    color: "#aecbe8"
                    text: display
                    renderType: Text.QtRendering
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}
