import QtQuick

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
        // Assign the source explicitly from reloadImage(). This makes an
        // avatar update observable even when the image-provider URL keeps the
        // same logical contact id.
        source: ""
        cache: false
        asynchronous: false
    }

    function reloadImage() {
        img.source = ""
        img.source = root.source ? root.source : ""
    }

    Component.onCompleted: reloadImage()

    Connections {
        target: root
        function onSourceChanged() {
            root.reloadImage()
        }
    }
}


/*##^## Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
 ##^##*/
