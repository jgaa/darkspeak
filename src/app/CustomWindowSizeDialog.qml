import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

UiDialog {
    title: qsTr("Custom window size")
    standardButtons: Dialog.Ok | Dialog.Cancel
    contentItem: GridLayout {
        columns: 2
        Label {
            text: qsTr("Width")
        }
        SpinBox {
            id: customWidth
            from: 320
            to: 16384
            value: mainWindow.width
            editable: true
        }
        Label {
            text: qsTr("Height")
        }
        SpinBox {
            id: customHeight
            from: 240
            to: 16384
            value: mainWindow.height
            editable: true
        }
    }
    onAccepted: appearance.resizeWindow(mainWindow, customWidth.value, customHeight.value)
}
