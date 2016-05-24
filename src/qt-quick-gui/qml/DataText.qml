import QtQuick 2.4
import QtQuick.Controls 1.3


Rectangle {
    id: root
    height: txt.contentHeight + 4
    color: "white"
    property alias text: txt.text
    property alias wrapMode: txt.wrapMode
    width: 200

    TextArea {
        id: txt
        width: parent.width
        readOnly: false
        //cursorVisible: true
        clip: true
        selectByMouse: true
        wrapMode: Text.NoWrap
        height: parent.height
    }
}
