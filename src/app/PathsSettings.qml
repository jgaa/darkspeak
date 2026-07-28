import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import org.darkspeak 1.0

Item {
    id: root
    implicitHeight: content.implicitHeight

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
        id: content
        width: parent.width
        height: implicitHeight
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
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
                        const selected = fileDialogs.openFile(
                            qsTr("Select database"), database.text)
                        if (selected)
                            database.text = selected
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
                        const selected = fileDialogs.openFolder(
                            qsTr("Download Home Path"), download.text)
                        if (selected)
                            download.text = selected
                    }
                }
            }
        }
    }
}
