import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
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
        id: list
        interactive: true
        model: identities
        anchors.fill: parent
        highlight: highlightBar

        delegate: Item {
            id: itemDelegate
            width: parent.width
            height: 112

            Row {
                id: row1
                anchors.fill: parent
                spacing: 8

                    Image {
                        id: avatar
                        height: 96
                        anchors.verticalCenter: parent.verticalCenter
                        width: 96
                        fillMode: Image.PreserveAspectFit
                        source: "qrc:///images/anonymous.svg"

                        Rectangle {
                            height: parent.width / 3
                            color: "#0219ca"
                            width: height
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 0
                            radius: width*0.5

                            Image {
                                id: torStatus
                                anchors.fill: parent
                                source: "qrc:///images/network_offline.svg"
                            }
                        }
                    }

                Column {
                    spacing: 10
                    Text {
                        font.pointSize: 14
                        color: itemDelegate.ListView.isCurrentItem ? "white" : "#9891f7"
                        text: name
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }

                    GridLayout {
                        rowSpacing: 0
                        rows: 3
                        flow: GridLayout.TopToBottom

                        Label { font.pointSize: 9; text: qsTr("Created")}
                        Label { font.pointSize: 9; text: qsTr("handle")}
                        Label { font.pointSize: 9; text: qsTr("Onion")}

                        Text {
                            font.pointSize: 9;
                            color: "#9891f7"
                            text: created
                        }

                        Text {
                            font.pointSize: 9;
                            color: "#9891f7"
                            text: handle
                        }

                        Text {
                            font.pointSize: 9;
                            color: "#9891f7"
                            text: onion
                        }
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: list.currentIndex = index
            }
        }
    }

    Component {
         id: highlightBar
         Rectangle {
             y: list.currentItem.y;
             anchors.fill: list.currentItem.width
             color: "#1a5b0d"
             //Behavior on y { SpringAnimation { spring: 2; damping: 0.1 } }
         }
     }
}
