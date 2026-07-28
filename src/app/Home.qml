import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.darkspeak 1.0

Page {
    width: 600
    height: 400
    id: root
    property color primaryText: "#243447"
    property color secondaryText: "#475569"
    property color labelText: "#64748b"
    property color selectedFill: "#294d70"
    property color selectedBorder: "#79c2f2"

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
            height: type === 1 ? 110 : 120

            Column {
                leftPadding: 10
                Row {
                    id: notificationFrame
                    topPadding: 4
                    Rectangle {
                        height: notificationLabel.height - 4
                        width: height
                        radius: width*0.5
                        color: itemDelegate.ListView.isCurrentItem ? "#ffffff" : root.primaryText

                        Image {
                            fillMode: Image.PreserveAspectFit
                            anchors.fill: parent
                            source: type === 1
                                    ? "qrc:///images/FileTansferActive.svg"
                                    : "qrc:///images/add-user.svg"

                        }
                    }
                    Text {
                        leftPadding: 10
                        id: notificationLabel
                        font.pointSize: 14
                        color: itemDelegate.ListView.isCurrentItem ? "#ffffff" : root.primaryText
                        text: type === 1 ? qsTr("Incoming File") : qsTr("New Contact Request")
                        font.bold: itemDelegate.ListView.isCurrentItem
                    }
                }

                GridLayout {
                    x: 16
                    rowSpacing: 0
                    rows: type === 1 ? 3 : 5
                    flow: GridLayout.TopToBottom
                    Label { visible: type !== 1; font.pointSize: 9; color: root.labelText; text: qsTr("Nick")}
                    Label { visible: type !== 1; font.pointSize: 9; color: root.labelText; text: qsTr("Message")}
                    Label { font.pointSize: 9; color: root.labelText; text: qsTr("When")}
                    Label { visible: type !== 1; font.pointSize: 9; color: root.labelText; text: qsTr("Handle")}
                    Label { visible: type !== 1; font.pointSize: 9; color: root.labelText; text: qsTr("Address")}

                    Text {
                        font.pointSize: 9;
                        color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                        text: nickName
                        visible: type !== 1
                    }

                    Text {
                        font.pointSize: 9;
                        color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                        text: message
                        visible: type !== 1
                    }

                    Text {
                        font.pointSize: 9;
                        color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                        text: when
                    }

                    Text {
                        font.pointSize: 9;
                        color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                        text: handle
                        visible: type !== 1
                    }

                    Text {
                        font.pointSize: 9;
                        color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                        text: address
                        visible: type !== 1
                    }

                    Text {
                        visible: type === 1
                        font.pointSize: 9
                        color: itemDelegate.ListView.isCurrentItem ? "#e6f1fb" : root.secondaryText
                        text: fileName + " (" + fileSize + " bytes)"
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
                    if (type === 1) {
                        fileActions.open()
                    } else {
                        confirmNewContact.message = message
                        confirmNewContact.open();
                    }
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
           color: "#263746"

           Text {
               text: section
               font.bold: true
               font.pointSize: 16
               color: root.primaryText
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
             color: root.selectedFill
             border.color: root.selectedBorder
             Behavior on y { SpringAnimation { spring: 1; damping: 0.1 } }
         }
    }

    MessageDialog {
        id: confirmNewContact
        property string message: ""
        title: qsTr("Accept Contact")
        text: qsTr("Do you accept this Contact Request?")
        buttons: MessageDialog.Yes | MessageDialog.No | MessageDialog.Cancel
        detailedText: qsTr("Message from contact: ") + message
        onButtonClicked: function(button) {
            if (button === MessageDialog.Yes) list.acceptContact(true)
            else if (button === MessageDialog.No) list.acceptContact(false)
        }
    }

    MessageDialog {
        id: fileActions
        title: qsTr("Incoming File")
        text: qsTr("Accept this file?")
        buttons: MessageDialog.Yes | MessageDialog.No | MessageDialog.Cancel
        onButtonClicked: function(button) {
            if (button === MessageDialog.Yes) notifications.acceptFile(list.currentIndex, true)
            else if (button === MessageDialog.No) notifications.acceptFile(list.currentIndex, false)
        }
    }
}
