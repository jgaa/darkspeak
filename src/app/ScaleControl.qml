import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    objectName: "scaleControl"
    spacing: 2 * appearance.factor
    ScaledToolButton {
        objectName: "zoomOutButton"
        text: "−"
        Accessible.name: qsTr("Zoom out")
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Zoom out")
        enabled: appearance.scale > 20
        onClicked: appearance.zoom(-1)
    }
    ScaleSelector {
        objectName: "scalePercentage"
        Layout.preferredWidth: implicitWidth
        Layout.maximumWidth: implicitWidth
    }
    ScaledToolButton {
        objectName: "zoomInButton"
        text: "+"
        Accessible.name: qsTr("Zoom in")
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Zoom in")
        enabled: appearance.scale < 200
        onClicked: appearance.zoom(1)
    }
}
