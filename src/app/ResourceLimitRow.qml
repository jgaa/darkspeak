import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    required property string label
    property alias from: valueSlider.from
    property alias to: valueSlider.to
    property alias value: valueSlider.value
    Layout.fillWidth: true

    Label { text: parent.label; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    Slider {
        id: valueSlider
        Layout.preferredWidth: 190
        stepSize: 1
        snapMode: Slider.SnapAlways
    }
    Label {
        text: Math.round(valueSlider.value)
        Layout.preferredWidth: 48
        horizontalAlignment: Text.AlignRight
    }
}
