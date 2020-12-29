import QtQuick 2.2
import QtQuick.Controls 1.1
import QtQuick.Layouts 1.3
import Qt.labs.settings 1.0

Item {
    id: root

    Settings {
        id: settings
        property int torMode: 0
        property string torCmd: "tor"
        property string torCtlHost
        property int torCtlPort: 9051
        property string torCtlPasswd
        property int torServiceFromPort: 1025
        property int torServiceToPort: 29999
        property string torAppHost
        property int torCtlAuthMode: 0
    }

    function commit() {
        settings.torMode = torMode.currentIndex
        settings.torCmd = torPath.text
        settings.torCtlHost = host.text
        settings.torCtlPort = port.value
        settings.torCtlPasswd = passwd.text
        settings.torCtlAuthMode = auth.currentIndex

        if (torMode.currentIndex == 0) {
            // Host
            settings.torServiceFromPort = prangeFrom.value
            settings.torServiceToPort = prangeTo.value
            settings.torAppHost = appHost.text
        } else {
            // Embedded
            settings.torServiceFromPort = ePrangeFrom.value
            settings.torServiceToPort = ePrangeTo.value
            settings.torAppHost = eAppHost.text
        }
    }

    ColumnLayout {
        id: topLayout
        anchors.fill: parent
        spacing: 4

        GridLayout {
            id: mode
            Layout.fillWidth: parent.width
            rowSpacing: 4
            rows: 1

            Label { font.pointSize: 9; text: qsTr("Mode")}
            ComboBox {
                id: torMode
                model: ["Host", "Embedded"]
                Layout.fillWidth: true
                currentIndex: settings.torMode
            }
        }

        StackLayout {
            currentIndex: torMode.currentIndex

            // Host
            GridLayout {
                id: fields
                Layout.fillWidth: parent.width
                rowSpacing: 4
                rows: 7
                flow: GridLayout.TopToBottom

                Label { font.pointSize: 9; text: qsTr("Tor Host")}
                Label { font.pointSize: 9; text: qsTr("Tor Port")}
                Label { font.pointSize: 9; text: qsTr("App's Host")}
                Label { font.pointSize: 9; text: qsTr("Authentication")}
                Label { font.pointSize: 9; text: qsTr("Password")}
                Label { font.pointSize: 9; text: qsTr("Port range from")}
                Label { font.pointSize: 9; text: qsTr("Port range to")}

                TextField {
                    id: host
                    placeholderText: qsTr("localhost")
                    Layout.fillWidth: true
                    text: settings.torCtlHost
                }

                SpinBox {
                    id: port
                    maximumValue: 65535
                    minimumValue: 1
                    width: 160
                    value: settings.torCtlPort
                }

                TextField {
                    id: appHost
                    placeholderText: qsTr("localhost")
                    Layout.fillWidth: true
                    text: settings.torAppHost
                }

                ComboBox {
                    id: auth
                    model: ["SafeCookie", "Password"]
                    Layout.fillWidth: true
                    currentIndex: settings.torCtlAuthMode
                }

                TextField {
                    id: passwd
                    echoMode: 2
                    Layout.fillWidth: true
                    enabled: auth.currentIndex === 1
                }

                SpinBox {
                    id: prangeFrom
                    maximumValue: 65535
                    minimumValue: 1025
                    width: 160
                    value: settings.torServiceFromPort
                }

                SpinBox {
                    id: prangeTo
                    width: 160
                    maximumValue: 65535
                    minimumValue: 1025
                    value: settings.torServiceToPort
                }
            }

            // Embedded
            GridLayout {
                Layout.fillWidth: parent.width
                rowSpacing: 4
                rows: 4
                flow: GridLayout.TopToBottom

                Label { font.pointSize: 9; text: qsTr("Tor command")}
                Label { font.pointSize: 9; text: qsTr("App's Host")}
                Label { font.pointSize: 9; text: qsTr("Port range from")}
                Label { font.pointSize: 9; text: qsTr("Port range to")}

                TextField {
                    id: torPath
                    placeholderText: qsTr("tor")
                    Layout.fillWidth: true
                    text: settings.torCmd
                }

                TextField {
                    id: eAppHost
                    placeholderText: qsTr("localhost")
                    Layout.fillWidth: true
                    text: settings.torAppHost
                }

                SpinBox {
                    id: ePrangeFrom
                    maximumValue: 65535
                    minimumValue: 1025
                    width: 160
                    value: settings.torServiceFromPort
                }

                SpinBox {
                    id: ePrangeTo
                    width: 160
                    maximumValue: 65535
                    minimumValue: 1025
                    value: settings.torServiceToPort
                }
            }
        }
    }
}
