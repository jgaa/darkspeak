import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T

T.ComboBox {
    id: control
    implicitWidth: 72 * appearance.factor
    implicitHeight: 28 * appearance.factor
    leftPadding: 6 * appearance.factor
    rightPadding: 20 * appearance.factor
    topPadding: 4 * appearance.factor
    bottomPadding: 4 * appearance.factor
    font: appearance.font
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    editable: false
    currentIndex: model.indexOf(appearance.scale + "%")
    model: ["20%", "50%", "75%", "100%", "125%", "150%", "200%"]
    displayText: appearance.scale + "%"
    Accessible.name: qsTr("UI scale percentage")
    onActivated: appearance.scale = parseInt(currentText)
    ToolTip.visible: hovered
    ToolTip.text: qsTr("Double-click the number to reset to 100%. Use the arrow to choose a scale.")
    contentItem: Text {
        objectName: "scalePercentageLabel"
        text: control.displayText
        textFormat: Text.PlainText
        font: control.font
        color: control.enabled ? appearance.text : appearance.disabled
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Text {
        objectName: "scaleIndicator"
        x: control.width - width - 6 * appearance.factor
        y: (control.height - height) / 2
        width: 12 * appearance.factor
        height: 16 * appearance.factor
        text: "▾"
        font: control.font
        color: control.enabled ? appearance.text : appearance.disabled
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        color: control.down || control.hovered ? appearance.hover : appearance.raised
        radius: appearance.cornerRadius
        border.color: control.activeFocus ? appearance.accent : appearance.border
        border.width: Math.max(0.5, appearance.factor)
    }
    MouseArea {
        id: resetArea
        objectName: "scaleResetArea"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width * 0.6
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        onPressed: control.forceActiveFocus(Qt.MouseFocusReason)
        // Consume the first click so it cannot open a popup and capture the
        // second click. The right-hand arrow keeps the ComboBox interaction.
        onDoubleClicked: function (mouse) {
            mouse.accepted = true;
            control.popup.close();
            appearance.scale = 100;
        }
    }
    delegate: T.ItemDelegate {
        id: entry
        required property int index
        required property string modelData
        width: control.popup.availableWidth
        implicitHeight: 28 * appearance.factor
        padding: 6 * appearance.factor
        font: control.font
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: entry.modelData
            textFormat: Text.PlainText
            font: entry.font
            color: entry.highlighted ? appearance.selectedText : appearance.text
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: entry.highlighted ? appearance.selected : entry.hovered ? appearance.hover : appearance.surface
            radius: appearance.cornerRadius
        }
    }
    popup: T.Popup {
        y: control.height + 2 * appearance.factor
        width: control.width
        padding: 4 * appearance.factor
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        height: Math.min(implicitHeight, Math.max(1, (control.Window.window ? control.Window.window.height : implicitHeight) - 16 * appearance.factor))
        focus: true
        closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
        contentItem: ListView {
            objectName: "scalePresetList"
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
        }
        background: Rectangle {
            color: appearance.surface
            border.color: appearance.border
            border.width: Math.max(0.5, appearance.factor)
            radius: appearance.cornerRadius
        }
    }
}
