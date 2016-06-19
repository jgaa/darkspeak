import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Dialogs 1.1
import com.jgaa.darkspeak 1.0

Component {
    id: root

    Rectangle {
        id: main_rect
        width: parent.width
        height: 52
        border.color: "#7cd5dd"
        border.width: 0
        radius: 4
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#ffffff" }
            GradientStop { position: 1.0; color: "#d7d7d7" }
        }

        function openChatWindow() {
            main_pane.openChatWindow(contactsModel.getMessagesModel(index))
        }

       EditBuddy{
            id: edit
       }

       MessageDialog {
            id: confirm_delete
            title: "Confirmation"
            icon: StandardIcon.Warning
            text: "Do you really want to delete "
                + nickname + "?"
            standardButtons: StandardButton.Yes | StandardButton.Cancel
            onYes: contactsModel.remove(index)
       }


       Menu {
            id: contextMenu

            MenuItem {
                text: "Chat"
                onTriggered: openChatWindow()
            }

            MenuItem {
                text: "Edit"
                onTriggered: {
                    edit.buddy = contactsModel.getContactData(index)
                    edit.open()
                }
            }

            MenuItem {
                text: "Delete"
                onTriggered: confirm_delete.open()
            }

            MenuItem {
                text: "Copy id to clipboard"
                onTriggered: {
                    darkRoot.copyToClipboard(handle)
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onDoubleClicked:  {
                openChatWindow()
            }
            onClicked: {
                if (mouse.button == Qt.RightButton) {
                    console.log("Right")
                    contextMenu.popup()
                }
            }

            onPressAndHold:contextMenu.popup()
            onEntered: main_rect.border.width = 2
            onExited: main_rect.border.width = 0
        }

        Row {
            id: contact_row
           height: contact_col.height > contact_icon.height ?
                       contact_col.height : contact_icon.height
           width: parent.width
           spacing: 4

           Image {
               id: contact_icon
               source: "qrc:/images/anon_contact_48x48.png"
           }

           Column {
               id: contact_col
               spacing: 4
               width: parent.width

               Row {
                   spacing: 4

                   Text {
                       id: contact_nickname
                       text: nickname
                       font.pointSize: 12
                       color: "steelblue"
                   }

                   Text {
                       anchors.bottom: contact_nickname.bottom
                       text: handle
                       font.pointSize: 8
                   }
               }

               Row {
                   spacing: 4

                   Text {
                       text: status
                       color: status_color
                       font.pointSize: 8
                   }

                   Text {
                       text: " Last seen: "
                       color: "grey"
                       font.pointSize: 8
                   }

                   Text {
                       text: last_seen
                       font.pointSize: 8
                   }
               }
           }
        }
    }
}
