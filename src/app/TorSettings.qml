import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

Item {
    id: root
    implicitHeight: topLayout.implicitHeight

    property var settings: settingsSession.values

    function commit() {
        settings.torMode = torMode.currentIndex;
        settings.torCmd = torPath.text;
        settings.torCtlHost = host.text;
        settings.torCtlPort = port.value;
        settings.torCtlPasswd = passwd.text;
        settings.torSocksHost = socksHost.text;
        settings.torSocksPort = socksPort.value;
        settings.torCtlAuthMode = auth.currentIndex;

        if (torMode.currentIndex == 0) {
            // Host
            settings.torServiceFromPort = prangeFrom.value;
            settings.torServiceToPort = prangeTo.value;
            settings.torAppHost = appHost.text;
        } else {
            // Embedded
            settings.torServiceFromPort = ePrangeFrom.value;
            settings.torServiceToPort = ePrangeTo.value;
            settings.torAppHost = eAppHost.text;
        }
    }

    ColumnLayout {
        id: topLayout
        width: parent.width
        height: implicitHeight
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: appearance.smallSpacing

        GridLayout {
            id: mode
            Layout.fillWidth: true
            rowSpacing: appearance.smallSpacing
            rows: 1

            Label {
                textFormat: Text.PlainText
                font: appearance.captionFont
                text: qsTr("Mode")
            }
            ComboBox {
                id: torMode
                model: [qsTr("System Tor"), qsTr("Private Tor")]
                Layout.fillWidth: true
                currentIndex: settings.torMode
            }
        }

        StackLayout {
            currentIndex: torMode.currentIndex

            // Host
            GridLayout {
                id: fields
                Layout.fillWidth: true
                rowSpacing: appearance.smallSpacing
                rows: 9
                flow: GridLayout.TopToBottom

                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Tor Host")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Tor Port")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("SOCKS Host")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("SOCKS Port")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("App's Host")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Authentication")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Password")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Port range from")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Port range to")
                }

                TextField {
                    id: host
                    placeholderText: qsTr("localhost")
                    Layout.fillWidth: true
                    text: settings.torCtlHost
                }

                SpinBox {
                    id: port
                    to: 65535
                    from: 1
                    width: 160 * appearance.factor
                    value: settings.torCtlPort
                }

                TextField {
                    id: socksHost
                    placeholderText: qsTr("localhost")
                    Layout.fillWidth: true
                    text: settings.torSocksHost
                }

                SpinBox {
                    id: socksPort
                    to: 65535
                    from: 1
                    width: 160 * appearance.factor
                    value: settings.torSocksPort
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
                    text: settings.torCtlPasswd
                    echoMode: TextInput.Password
                    Layout.fillWidth: true
                    enabled: auth.currentIndex === 1
                }

                SpinBox {
                    id: prangeFrom
                    to: 65535
                    from: 1025
                    width: 160 * appearance.factor
                    value: settings.torServiceFromPort
                }

                SpinBox {
                    id: prangeTo
                    width: 160 * appearance.factor
                    to: 65535
                    from: 1025
                    value: settings.torServiceToPort
                }
            }

            // Embedded
            GridLayout {
                Layout.fillWidth: true
                rowSpacing: appearance.smallSpacing
                rows: 4
                flow: GridLayout.TopToBottom

                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Tor command")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("App's Host")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Port range from")
                }
                Label {
                    textFormat: Text.PlainText
                    font: appearance.captionFont
                    text: qsTr("Port range to")
                }

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
                    to: 65535
                    from: 1025
                    width: 160 * appearance.factor
                    value: settings.torServiceFromPort
                }

                SpinBox {
                    id: ePrangeTo
                    width: 160 * appearance.factor
                    to: 65535
                    from: 1025
                    value: settings.torServiceToPort
                }
            }
        }
    }
}
