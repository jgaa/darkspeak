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
        id: list
        interactive: true
        model: identities
        anchors.fill: parent
        highlight: highlightBar

        delegate: Item {
            id: itemDelegate
            width: parent.width
            height: 40
            Row {
                spacing: 10
                Text {
                    //color: "#adadc5"

                    color: itemDelegate.ListView.isCurrentItem ? "white" : "#9891f7"
                    text: name
                    font.bold: itemDelegate.ListView.isCurrentItem
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    color: "#9891f7"
                    text: created
                    anchors.verticalCenter: parent.verticalCenter
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
             y: listView.currentItem.y;
             anchors.fill: listView.currentItem.width
             color: "#1a5b0d"
             Behavior on y { SpringAnimation { spring: 2; damping: 0.1 } }
         }
     }
}
