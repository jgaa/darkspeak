import QtQuick
import QtQuick.Effects

Rectangle {
    id: root

    property bool rounded: true
    property bool adapt: true
    property var source

    radius: width*0.5

    Image {
        id: img
        height: parent.height - 6
        width: parent.width - 6
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: 0
        anchors.horizontalCenterOffset: 0
        fillMode: Image.PreserveAspectFit
        source: root.source ? root.source : ""
        cache: false
        layer.enabled: root.rounded
        layer.effect: MultiEffect {
            maskEnabled: true
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
            maskSource: Item {
                width: img.width
                height: img.height
                Rectangle {
                    anchors.centerIn: parent
                    width: root.adapt ? img.width : Math.min(img.width, img.height)
                    height: root.adapt ? img.height : width
                    radius: Math.min(width, height)
                }
            }
        }
    }
}


/*##^## Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
 ##^##*/
