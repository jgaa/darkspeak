import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import QtQuick.Dialogs 1.3
import com.jgaa.darkspeak 1.0

Page {
    width: 600
    height: 400
    id: root

    header: Label {
        id: label
        text: qsTr("Home")
        font.pixelSize: Qt.application.font.pixelSize * 2
        padding: 10
    }

    ListView {
        id: list
        anchors.fill: parent
        model: notifications
        section.property: "identityName"
        section.delegate: sectionHeading
        highlight: highlightBar
        delegate: Item {
            id: itemDelegate
            width: parent.width - 10
            height: 120

            Column {
                leftPadding: 10
                Row {
                    id: notificationFrame
                    topPadding: 4
                    Rectangle {
                        height: notificationLabel.height - 4
                        width: height
                        radius: width*0.5
                        color: "white"

                        Image {
                            fillMode: Image.PreserveAspectFit
                            anchors.fill: parent
                            source: "qrc:///images/add-user.svg"

                        }
                    }
                    Text {
                        leftPadding: 10
                        id: notificationLabel
                        font.pointSize: 14
                        color: "white"
                        text: qsTr("New Contact Request")
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }
                }

                GridLayout {
                    x: 16
                    rowSpacing: 0
                    rows: 5
                    flow: GridLayout.TopToBottom
                    Label { font.pointSize: 9; text: qsTr("Nick")}
                    Label { font.pointSize: 9; text: qsTr("Message")}
                    Label { font.pointSize: 9; text: qsTr("When")}
                    Label { font.pointSize: 9; text: qsTr("Handle")}
                    Label { font.pointSize: 9; text: qsTr("Address")}

                    Text {
                        font.pointSize: 9;
                        color: "skyblue"
                        text: nickName
                    }

                    Text {
                        font.pointSize: 9;
                        color: "skyblue"
                        text: message
                    }

                    Text {
                        font.pointSize: 9;
                        color: "skyblue"
                        text: when
                    }

                    Text {
                        font.pointSize: 9;
                        color: "skyblue"
                        text: handle
                    }

                    Text {
                        font.pointSize: 9;
                        color: "skyblue"
                        text: address
                    }

                    Row {
                        anchors.fill: parent
                        spacing: 4
                    }
                }
            }

            MouseArea {
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                anchors.fill: parent
                onClicked: {
                    list.currentIndex = index
                    if (mouse.button === Qt.RightButton) {
                        takeAction()
                    }
                }

                onPressAndHold: {
                    list.currentIndex = index
                    takeAction()
                }

                function takeAction() {
                    confirmNewContact.message = message
                    confirmNewContact.open();
                }
            }
        }

        function acceptContact(accept) {
            notifications.acceptContact(currentIndex, accept)
        }
    }

    Component {
       id: sectionHeading
       Rectangle {
           width: root.width
           height: childrenRect.height
           color: "darkblue"

           Text {
               text: section
               font.bold: true
               font.pointSize: 16
               color: "white"
           }
       }
   }

   Component {
         id: highlightBar
         Rectangle {
             x: 8
             width: list.currentItem.width - 8
             radius: 5
             y: list.currentItem.y;
             color: "black"
             border.color: "yellow"
             Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
         }
    }

    MessageDialog {
        id: confirmNewContact
        property string message: ""
        title: qsTr("Accept Contact")
        text: qsTr("Do you accept this Contact Request?")
        standardButtons: MessageDialog.Yes | MessageDialog.No | MessageDialog.Cancel
        detailedText: qsTr("Message from contact: ") + message
        onYes: list.acceptContact(true)
        onNo: list.acceptContact(false)
    }
}
