import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: harness
    width: 1000
    height: 700
    visible: true
    title: qsTr("DarkSpeak UI fixtures")
    // Bind roles individually: Popup copies whole Palette objects on assignment.
    palette.active.alternateBase: appearance.paletteColors.active.alternateBase
    palette.active.base: appearance.paletteColors.active.base
    palette.active.brightText: appearance.paletteColors.active.brightText
    palette.active.button: appearance.paletteColors.active.button
    palette.active.buttonText: appearance.paletteColors.active.buttonText
    palette.active.dark: appearance.paletteColors.active.dark
    palette.active.highlight: appearance.paletteColors.active.highlight
    palette.active.highlightedText: appearance.paletteColors.active.highlightedText
    palette.active.light: appearance.paletteColors.active.light
    palette.active.link: appearance.paletteColors.active.link
    palette.active.linkVisited: appearance.paletteColors.active.linkVisited
    palette.active.mid: appearance.paletteColors.active.mid
    palette.active.midlight: appearance.paletteColors.active.midlight
    palette.active.shadow: appearance.paletteColors.active.shadow
    palette.active.text: appearance.paletteColors.active.text
    palette.active.toolTipBase: appearance.paletteColors.active.toolTipBase
    palette.active.toolTipText: appearance.paletteColors.active.toolTipText
    palette.active.window: appearance.paletteColors.active.window
    palette.active.windowText: appearance.paletteColors.active.windowText
    palette.active.placeholderText: appearance.paletteColors.active.placeholderText
    palette.active.accent: appearance.paletteColors.active.accent
    palette.inactive.alternateBase: appearance.paletteColors.inactive.alternateBase
    palette.inactive.base: appearance.paletteColors.inactive.base
    palette.inactive.brightText: appearance.paletteColors.inactive.brightText
    palette.inactive.button: appearance.paletteColors.inactive.button
    palette.inactive.buttonText: appearance.paletteColors.inactive.buttonText
    palette.inactive.dark: appearance.paletteColors.inactive.dark
    palette.inactive.highlight: appearance.paletteColors.inactive.highlight
    palette.inactive.highlightedText: appearance.paletteColors.inactive.highlightedText
    palette.inactive.light: appearance.paletteColors.inactive.light
    palette.inactive.link: appearance.paletteColors.inactive.link
    palette.inactive.linkVisited: appearance.paletteColors.inactive.linkVisited
    palette.inactive.mid: appearance.paletteColors.inactive.mid
    palette.inactive.midlight: appearance.paletteColors.inactive.midlight
    palette.inactive.shadow: appearance.paletteColors.inactive.shadow
    palette.inactive.text: appearance.paletteColors.inactive.text
    palette.inactive.toolTipBase: appearance.paletteColors.inactive.toolTipBase
    palette.inactive.toolTipText: appearance.paletteColors.inactive.toolTipText
    palette.inactive.window: appearance.paletteColors.inactive.window
    palette.inactive.windowText: appearance.paletteColors.inactive.windowText
    palette.inactive.placeholderText: appearance.paletteColors.inactive.placeholderText
    palette.inactive.accent: appearance.paletteColors.inactive.accent
    palette.disabled.alternateBase: appearance.paletteColors.disabled.alternateBase
    palette.disabled.base: appearance.paletteColors.disabled.base
    palette.disabled.brightText: appearance.paletteColors.disabled.brightText
    palette.disabled.button: appearance.paletteColors.disabled.button
    palette.disabled.buttonText: appearance.paletteColors.disabled.buttonText
    palette.disabled.dark: appearance.paletteColors.disabled.dark
    palette.disabled.highlight: appearance.paletteColors.disabled.highlight
    palette.disabled.highlightedText: appearance.paletteColors.disabled.highlightedText
    palette.disabled.light: appearance.paletteColors.disabled.light
    palette.disabled.link: appearance.paletteColors.disabled.link
    palette.disabled.linkVisited: appearance.paletteColors.disabled.linkVisited
    palette.disabled.mid: appearance.paletteColors.disabled.mid
    palette.disabled.midlight: appearance.paletteColors.disabled.midlight
    palette.disabled.shadow: appearance.paletteColors.disabled.shadow
    palette.disabled.text: appearance.paletteColors.disabled.text
    palette.disabled.toolTipBase: appearance.paletteColors.disabled.toolTipBase
    palette.disabled.toolTipText: appearance.paletteColors.disabled.toolTipText
    palette.disabled.window: appearance.paletteColors.disabled.window
    palette.disabled.windowText: appearance.paletteColors.disabled.windowText
    palette.disabled.placeholderText: appearance.paletteColors.disabled.placeholderText
    palette.disabled.accent: appearance.paletteColors.disabled.accent
    font: appearance.font
    color: appearance.window
    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            Label {
                text: qsTr("Component review")
                Layout.fillWidth: true
            }
            ComboBox {
                model: appearance.themeNames
                currentIndex: appearance.themeIndex
                onActivated: appearance.themeIndex = currentIndex
                Accessible.name: qsTr("Theme")
            }
            ComboBox {
                model: [qsTr("Automatic"), qsTr("Light"), qsTr("Dark")]
                currentIndex: appearance.mode
                onActivated: appearance.mode = currentIndex
                Accessible.name: qsTr("Appearance")
            }
            ScaleControl {}
        }
    }
    ListView {
        id: history
        objectName: "fixtureHistory"
        anchors.fill: parent
        anchors.margins: 12 * appearance.factor
        model: mockMessages
        clip: true
        cacheBuffer: Math.max(0, height)
        spacing: appearance.spacing
        ScrollBar.vertical: ScrollBar {}
        delegate: Item {
            required property string body
            required property bool outgoing
            width: history.width
            height: bubble.implicitHeight
            Component.onCompleted: mockMessages.delegateCreated()
            Component.onDestruction: mockMessages.delegateDestroyed()
            MessageBubble {
                id: bubble
                width: Math.max(1, Math.min(parent.width, 680 * appearance.factor))
                height: implicitHeight
                x: parent.outgoing ? parent.width - width : 0
                outgoing: parent.outgoing
                messageText: parent.body
                timestamp: qsTr("Today, 12:34")
                status: outgoing ? qsTr("Delivered") : qsTr("Received")
            }
        }
    }
}
