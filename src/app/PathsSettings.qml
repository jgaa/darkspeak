import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import Qt.labs.settings 1.0
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Item {
    id: root

    Settings {
        id: settings
        property string dbpath
        property string downloadLocation
    }

    function commit() {
        settings.dbpath = database.text
        settings.downloadLocation = download.text
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        GridLayout {
            id: fields
            Layout.fillWidth: true
            rowSpacing: 4
            rows: 2
            flow: GridLayout.TopToBottom

            Label { font.pointSize: 9; text: qsTr("Database")}
            Label { font.pointSize: 9; text: qsTr("Download")}

            Row {
                height: database.height
                Layout.fillWidth: true
                TextField {
                    id: database
                    text: settings.dbpath
                    anchors.right: databaseBtn.left
                    anchors.rightMargin: 6
                    anchors.left: parent.left
                    anchors.leftMargin: 0
                }

                Button {
                    id: databaseBtn
                    width: 45
                    text: "...";
                    anchors.right: parent.right
                    anchors.rightMargin: 0

                    onClicked: {
                        databaseSelector.open()
                    }
                }

                FileDialog {
                    id: databaseSelector
                    title: qsTr("Select database")
                    folder: manager.pathToUrl(database.text)

                    onAccepted: {
                        database.text = manager.urlToPath(fileUrl)
                    }
                }
            }

            Row {
                height: download.height
                Layout.fillWidth: true
                TextField {
                    id: download
                    text: settings.downloadLocation
                    anchors.right: downloadBtn.left
                    anchors.rightMargin: 6
                    anchors.left: parent.left
                    anchors.leftMargin: 0
                }

                Button {
                    id: downloadBtn
                    width: 45
                    text: "...";
                    anchors.right: parent.right
                    anchors.rightMargin: 0

                    onClicked: {
                        downloadSelector.open()
                    }
                }

                FileDialog {
                    id: downloadSelector
                    title: qsTr("Download Home Path")
                    folder: manager.pathToUrl(download.text)
                    selectFolder: true

                    onAccepted: {
                        download.text = manager.urlToPath(fileUrl)
                    }
                }
            }
        }
    }
}
