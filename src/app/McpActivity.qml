import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolButton {
    id: root
    property double nowTick: Date.now()
    Timer {
        interval: 1000
        repeat: true
        running: activity.visible
        onTriggered: root.nowTick = Date.now()
    }
    signal openSettings
    signal openApprovals
    text: qsTr("MCP activity")
    display: AbstractButton.IconOnly
    icon.source: Qt.resolvedUrl("images/mcp.svg")
    icon.color: appearance.text
    icon.width: 20 * appearance.factor
    icon.height: 20 * appearance.factor
    implicitWidth: 28 * appearance.factor
    implicitHeight: 28 * appearance.factor
    ToolTip.visible: hovered
    ToolTip.text: text
    Accessible.name: qsTr("MCP activity")
    onClicked: activity.open()
    SequentialAnimation on opacity {
        running: !!mcp && mcp.active
        loops: Animation.Infinite
        NumberAnimation { to: 0.4; duration: 600 }
        NumberAnimation { to: 1; duration: 600 }
        onStopped: root.opacity = 1
    }
    Popup {
        id: activity
        objectName: "mcpActivityPopup"
        parent: root.Overlay.overlay
        property real edgeMargin: 8 * appearance.factor
        property point buttonAnchor: Qt.point(0, 0)
        function updateAnchor() {
            if (parent)
                buttonAnchor = root.mapToItem(parent, root.width, 0);
        }
        onAboutToShow: updateAnchor()
        width: Math.min(520 * appearance.factor, Math.max(0, (parent ? parent.width : 0) - 2 * edgeMargin))
        height: Math.min(450 * appearance.factor, Math.max(0, (parent ? parent.height : 0) - 2 * edgeMargin))
        x: Math.max(edgeMargin, Math.min(buttonAnchor.x - width, (parent ? parent.width : 0) - width - edgeMargin))
        y: Math.max(edgeMargin, Math.min(buttonAnchor.y - height - edgeMargin, (parent ? parent.height : 0) - height - edgeMargin))
        Connections {
            target: activity.parent
            function onWidthChanged() {
                if (activity.visible)
                    Qt.callLater(activity.updateAnchor);
            }
            function onHeightChanged() {
                if (activity.visible)
                    Qt.callLater(activity.updateAnchor);
            }
        }
        ColumnLayout {
            anchors.fill: parent
            Label {
                text: qsTr("MCP activity")
                font.bold: true
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: mcp ? mcp.activity : []
                delegate: Label {
                    required property var modelData
                    width: ListView.view.width
                    text: modelData.key + " · " + modelData.tool + "\n"
                        + modelData.identity + " · " + (modelData.contacts || []).join(", ") + " · " + (modelData.state || qsTr("Pending approval"))
                        + (modelData.time ? " · " + Math.max(0, Math.floor((root.nowTick - new Date(modelData.time).getTime()) / 1000)) + qsTr("s ago") : "")
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    padding: appearance.smallSpacing
                    color: modelData.state === "Completed" ? appearance.text : modelData.state ? appearance.warning : appearance.secondaryText
                }
            }
            Button {
                text: qsTr("Approvals on Home")
                onClicked: { activity.close(); root.openApprovals(); }
            }
            Button {
                text: qsTr("MCP settings, keys and policies")
                onClicked: { activity.close(); root.openSettings(); }
            }
        }
    }
}
