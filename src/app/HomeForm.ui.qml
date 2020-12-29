import QtQuick 2.12
import QtQuick.Controls 2.5
import com.jgaa.darkspeak 1.0

Page {
    width: 600
    height: 400

    header: Label {
        id: label
        text: qsTr("Home")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10

        ToolButton {
            id: onlineStatusBtn
            x: 400
            y: 8
            text: qsTr("Tool Button")
            display: AbstractButton.IconOnly
            anchors.verticalCenterOffset: -4
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 8
            icon.source: manager.onlineStatusIcon
        }
    }

    Row {
        id: row
        anchors.fill: parent
    }

    ListView {
        id: listView
        anchors.fill: parent
        delegate: Item {
            x: 5
            width: 80
            height: 40
            Row {
                id: row1
                spacing: 10
                Rectangle {
                    width: 40
                    height: 40
                    color: colorCode
                }

                Text {
                    text: name
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
        model: ListModel {
            ListElement {
                name: "Grey"
                colorCode: "grey"
            }

            ListElement {
                name: "Red"
                colorCode: "red"
            }

            ListElement {
                name: "Blue"
                colorCode: "blue"
            }

            ListElement {
                name: "Green"
                colorCode: "green"
            }
        }
    }
}
