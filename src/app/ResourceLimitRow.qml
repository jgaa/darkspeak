import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    required property string label
    property alias from: valueSlider.from
    property alias to: valueSlider.to
    property alias value: valueSlider.value
    Layout.fillWidth: true

    Label {
        textFormat: Text.PlainText
        text: parent.label
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
    Slider {
        id: valueSlider
        Layout.preferredWidth: 190 * appearance.factor
        stepSize: 1
        snapMode: Slider.SnapAlways
    }
    Label {
        textFormat: Text.PlainText
        text: Math.round(valueSlider.value)
        Layout.preferredWidth: 48 * appearance.factor
        horizontalAlignment: Text.AlignRight
    }
}
