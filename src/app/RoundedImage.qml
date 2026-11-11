import QtQuick

Rectangle {
    id: root

    property bool rounded: true
    property bool adapt: true
    property var source

    color: appearance.raised
    border.color: appearance.border
    radius: width * 0.5

    Image {
        id: img
        height: Math.max(1, parent.height - 4 * appearance.factor)
        width: Math.max(1, parent.width - 4 * appearance.factor)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: 0
        anchors.horizontalCenterOffset: 0
        fillMode: Image.PreserveAspectFit
        // Assign the source explicitly from reloadImage(). This makes an
        // avatar update observable even when the image-provider URL keeps the
        // same logical contact id.
        source: ""
        cache: false
        asynchronous: false
    }

    function reloadImage() {
        img.source = "";
        img.source = root.source ? root.source : appearance.asset("avatar");
    }

    Component.onCompleted: reloadImage()

    Connections {
        target: root
        function onSourceChanged() {
            root.reloadImage();
        }
    }
}

/*##^## Designer {
    D{i:0;autoSize:true;height:480 * appearance.factor;width:640 * appearance.factor}
}
 ##^##*/
