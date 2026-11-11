import QtQuick
import org.darkspeak 1.0

Item {
    width: cList.width
    height: bubble.implicitHeight + 6 * appearance.factor
    MessageBubble {
        id: bubble
        width: Math.max(1, Math.min(parent.width - 24 * appearance.factor, 680 * appearance.factor))
        height: implicitHeight
        x: outgoing ? parent.width - width - 8 * appearance.factor : 8 * appearance.factor
        messageText: cData.content || ""
        outgoing: cData.direction === Message.OUTGOING
        timestamp: cData.composedTime ? Qt.formatDateTime(cData.composedTime, Qt.DefaultLocaleShortDate) : ""
        status: cData.stateName || ""
        failed: cData.messageState === Message.MS_REJECTED
    }
}
