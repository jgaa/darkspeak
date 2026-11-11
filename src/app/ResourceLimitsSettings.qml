import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

Item {
    id: root
    implicitHeight: content.implicitHeight

    property var settings: settingsSession.values

    function commit() {
        settings.resourceUnapprovedConnections = unapprovedConnections.value;
        settings.resourceOutstandingRequests = outstandingRequests.value;
        settings.resourcePendingConnectionRequests = pendingConnectionRequests.value;
        settings.resourceActiveFileTransfers = activeFileTransfers.value;
        settings.resourceQueuedFileOffers = queuedFileOffers.value;
        settings.resourceRetainedInterruptedTransfers = retainedInterruptedTransfers.value;
        settings.resourceAcceptedFileBytes = acceptedFileGiB.value * 1024 * 1024 * 1024;
        settings.resourceUnfinishedIncomingBytes = unfinishedIncomingGiB.value * 1024 * 1024 * 1024;
        settings.resourceIncomingDiskBytes = incomingDiskGiB.value * 1024 * 1024 * 1024;
        settings.resourceMinimumFreeSpacePercent = minimumFreeSpacePercent.value;
        settings.resourceSocketOutputBytes = socketOutputKiB.value * 1024;
    }

    ColumnLayout {
        id: content
        width: parent.width
        spacing: appearance.spacing

        Label {
            textFormat: Text.PlainText
            text: qsTr("Resource limits")
            font.bold: true
        }
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("These limits protect your device and connection. Values are restricted to the v3 protocol ceilings.")
        }

        ResourceLimitRow {
            id: unapprovedConnections
            label: qsTr("Unapproved connections")
            from: 1
            to: 32
            value: settings.resourceUnapprovedConnections
        }
        ResourceLimitRow {
            id: outstandingRequests
            label: qsTr("Outstanding requests per connection")
            from: 1
            to: 256
            value: settings.resourceOutstandingRequests
        }
        ResourceLimitRow {
            id: pendingConnectionRequests
            label: qsTr("Pending connection requests")
            from: 1
            to: 256
            value: settings.resourcePendingConnectionRequests
        }
        ResourceLimitRow {
            id: activeFileTransfers
            label: qsTr("Active file transfers per contact")
            from: 1
            to: 2
            value: settings.resourceActiveFileTransfers
        }
        ResourceLimitRow {
            id: queuedFileOffers
            label: qsTr("Queued file offers per peer")
            from: 1
            to: 64
            value: settings.resourceQueuedFileOffers
        }
        ResourceLimitRow {
            id: retainedInterruptedTransfers
            label: qsTr("Retained interrupted transfers per peer")
            from: 1
            to: 32
            value: settings.resourceRetainedInterruptedTransfers
        }
        ResourceLimitRow {
            id: acceptedFileGiB
            label: qsTr("Maximum accepted file size (GiB)")
            from: 1
            to: 1024
            value: settings.resourceAcceptedFileBytes / (1024 * 1024 * 1024)
        }
        ResourceLimitRow {
            id: unfinishedIncomingGiB
            label: qsTr("Unfinished incoming data per peer (GiB)")
            from: 1
            to: 2048
            value: settings.resourceUnfinishedIncomingBytes / (1024 * 1024 * 1024)
        }
        ResourceLimitRow {
            id: incomingDiskGiB
            label: qsTr("Maximum incoming transfer disk usage (GiB)")
            from: 1
            to: 50
            value: settings.resourceIncomingDiskBytes / (1024 * 1024 * 1024)
        }
        ResourceLimitRow {
            id: minimumFreeSpacePercent
            label: qsTr("Required available disk space (%)")
            from: 1
            to: 100
            value: settings.resourceMinimumFreeSpacePercent
        }
        ResourceLimitRow {
            id: socketOutputKiB
            label: qsTr("Socket output queue (KiB)")
            from: 16
            to: 256
            value: settings.resourceSocketOutputBytes / 1024
        }
    }
}
