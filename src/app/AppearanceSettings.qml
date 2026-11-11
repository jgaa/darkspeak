import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    spacing: 12 * appearance.factor
    Label {
        objectName: "appearanceHeading"
        text: qsTr("Appearance")
        font.bold: true
    }
    ComboBox {
        objectName: "appearanceModeSelector"
        Layout.fillWidth: true
        model: [qsTr("Automatic"), qsTr("Light"), qsTr("Dark")]
        currentIndex: appearance.mode
        onActivated: appearance.mode = currentIndex
        Accessible.name: qsTr("Appearance")
    }
    Label {
        text: qsTr("Theme")
        font.bold: true
    }
    ComboBox {
        objectName: "themeSelector"
        Layout.fillWidth: true
        model: appearance.themeNames
        currentIndex: appearance.themeIndex
        onActivated: appearance.themeIndex = currentIndex
        Accessible.name: qsTr("Theme")
    }
    Label {
        Layout.fillWidth: true
        text: appearance.themeDescription
        wrapMode: Text.WordWrap
    }
    Label {
        Layout.fillWidth: true
        visible: appearance.theme === "Native" && appearance.variantUnavailable
        color: appearance.warning
        text: qsTr("Your desktop does not provide the requested variant. Native is using the available desktop palette.")
        wrapMode: Text.WordWrap
    }
    Label {
        objectName: "darkOnlyNotice"
        Layout.fillWidth: true
        visible: appearance.darkOnly
        text: qsTr("This theme is available only in Dark. Your appearance preference is kept for other themes.")
        wrapMode: Text.WordWrap
        color: appearance.secondaryText
    }
    Label {
        text: qsTr("UI scale")
        font.bold: true
    }
    SpinBox {
        from: 20
        to: 200
        stepSize: 10
        value: appearance.scale
        editable: true
        onValueModified: appearance.scale = value
        Accessible.name: qsTr("UI scale percentage")
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Changes are previewed immediately. Save keeps them; Cancel restores the previous appearance.")
        wrapMode: Text.WordWrap
        color: appearance.secondaryText
    }
}
