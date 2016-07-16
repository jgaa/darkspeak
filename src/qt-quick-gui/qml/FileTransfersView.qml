
import QtQuick 2.4
import QtQuick.Controls 1.3

import com.jgaa.darkspeak 1.0

Item {
    id: root
    //height: 50
    property alias model: transfers.model
    property string stateName: "transfers"
    property int margin: 6
    property int iconSize: 16

    Component {
        id: transferDelegate

        Rectangle {
            id: data
            property int margin: 4
            x: 0
            width: root.width
            height: 20 + margin + 16
            radius: 4
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#ffffff" }
                GradientStop { position: 1.0; color: "#d7d7d7" }
            }

            Label {
                width: data.width
                text: name
            }

            Image {
                id: fileIcon
                height: iconSize
                width: iconSize
                anchors.bottom: data.bottom
                source: icon
            }

            Button {
                id: removeBtn
                anchors.bottom: data.bottom
                anchors.right: data.right
                tooltip: "Delete this file"
                height: iconSize
                width: iconSize
                iconSource: "qrc:/images/Delete.svg"
                onClicked: {
                    transfersModel.deleteTransfer(index)
                }
            }

            Button {
                id: folderBtn
                anchors.bottom: data.bottom
                anchors.left: fileIcon.right
                anchors.leftMargin: margin
                tooltip: "Open containing folder"
                height: iconSize
                width: iconSize
                iconSource: "qrc:/images/OpenFolder.svg"
                onClicked: {
                    transfersModel.openFolder(index)
                }
            }

            Label {
                id: sizeLabel
                text: size
                width: 80
                anchors.verticalCenter: folderBtn.verticalCenter
                anchors.right: removeBtn.left
            }

            ProgressBar {
                anchors.left: folderBtn.right
                anchors.right: sizeLabel.left
                anchors.verticalCenter: folderBtn.verticalCenter
                anchors.margins: margin
                value: (percent / 100.0)
            }

        }
    }

    Component {
    id: sectionDelegate

        Rectangle {
            width: transfers.width
            height: 25
            radius: 4
            gradient: Gradient {
                GradientStop { position: 0.0; color: "blue" }
                GradientStop { position: 1.0; color: "darkblue" }
            }

            Label {
                text: section
                color: "white"
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 4
            }
        }
    }

    ListView {
        id: transfers
        //width: root.width
        anchors.fill: root
        highlightRangeMode: ListView.NoHighlightRange
        spacing: 4
        clip: true
        delegate: transferDelegate
        section.property: "buddy_name"
        section.delegate: sectionDelegate

        onCountChanged: {
            console.log("onAddChanged: hit!")
            if (currentIndex === count - 2) {
                currentIndex = count - 1
            }
        }
        onModelChanged: {
            positionViewAtEnd()
            currentIndex = count - 1
        }
    }
}

