import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var whom
    property string text: ""
    implicitHeight: appearance.headerHeight
    height: implicitHeight
    RowLayout {
        anchors.fill: parent
        anchors.margins: appearance.spacing
        spacing: 10 * appearance.factor
        RoundedImage {
            visible: !!root.whom
            source: root.whom ? root.whom.avatarUrl : ""
            Layout.preferredWidth: 36 * appearance.factor
            Layout.preferredHeight: 36 * appearance.factor
        }
        Label {
            Layout.fillWidth: true
            text: root.text + (root.whom ? root.whom.name : "")
            textFormat: Text.PlainText
            color: appearance.text
            font: appearance.headingFont
            elide: Text.ElideRight
        }
    }
}
