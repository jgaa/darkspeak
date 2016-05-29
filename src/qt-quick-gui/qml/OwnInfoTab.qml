import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Layouts 1.2
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

/* struct Info {
        std::string id;
        Status status = Status::OFF_LINE;
        std::string profile_name;
        std::string profile_text;
    };
*/

Item {
    id: root

    property alias handle: handleId.text
    property alias status: statusId.currentIndex
    property alias nickname: profileName.text
    property alias profileText: profileTextId.text

    ScrollView {
        id: scroller
        anchors.fill: parent

        ColumnLayout {
            spacing: 8
            Layout.fillWidth: true
            Item { Layout.preferredHeight: 4 } // padding

            RowLayout {
                id: handleRow
                spacing: 6
                Label { id: handleLabel; text: "Chat ID:" }
                TextField {
                    id: handleId
                    text: settings.handle
                    Layout.alignment: Qt.AlignBaseline
                    Layout.fillWidth: true
                    placeholderText: "Tor Chat ID"
                }
            }

            RowLayout {
                spacing: 6
                Label { id: statusLabel; text: "Status:" }
                ComboBox {
                    id: statusId
                    Layout.fillWidth: true
                    currentIndex: settings.status
                    model: ListModel {
                       id: model
                       ListElement { text: "Available"; }
                       ListElement { text: "Away"; }
                       ListElement { text: "Extended Away"; }
                     }
                }
            }

            RowLayout {
                id: profileNameRow
                spacing: 6
                Label { id: profileNameLabel; text: "Nickname:" }
                TextField {
                    id: profileName
                    text: settings.nickname
                    Layout.alignment: Qt.AlignBaseline
                    Layout.fillWidth: true
                    placeholderText: "Your own nickname"
                }
            }

            RowLayout {
                id: profileInfoRow
                spacing: 6
                Label { id: profileInfoLabel; text: "Info:" }
                TextArea {
                    id: profileTextId
                    text: settings.profileText
                    height: 50
                    Layout.alignment: Qt.AlignBaseline
                    Layout.fillWidth: true
                }
            }
        }
    }
}
