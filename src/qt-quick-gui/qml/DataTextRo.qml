import QtQuick 2.4
import QtQuick.Controls 1.3


Rectangle {
    id: root
    height: txt.height + 4
    color: "silver"
    property alias text: txt.text
    property alias wrapMode: txt.wrapMode
    width: 200

    TextInput {
        id: txt
        width: parent.width
        readOnly: true
        clip: true
        selectByMouse: true
        wrapMode: Text.NoWrap
        height: contentHeight + 4
        color: "black"
    }
}
