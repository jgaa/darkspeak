import QtQuick 2.12
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Page {
    width: 600
    height: 400

    header: Label {
        text: qsTr("Your Identities")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }

    Row {
        id: row
        anchors.fill: parent
    }

    ListView {
        id: listView
        model: identities
        anchors.fill: parent
        delegate: Item {
            x: 5
            width: 80
            height: 40
            Row {
                id: row1
                spacing: 10

                Rectangle {
                    width: 32
                    height: 32
                    color: blue
                }

                Text {
                    color: "#9891f7"
                    text: model.name
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    color: "#9891f7"
                    text: created
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}
