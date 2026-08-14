import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

Item {
    id: root
    implicitHeight: content.implicitHeight

    Settings {
        id: settings
        property int unapprovedConnections: 8
        property int outstandingRequests: 64
        property int pendingConnectionRequests: 64
        property int activeFileTransfers: 2
        property int queuedFileOffers: 16
        property int retainedInterruptedTransfers: 8
        property double resourceAcceptedFileBytes: 10 * 1024 * 1024 * 1024
        property double resourceUnfinishedIncomingBytes: 20 * 1024 * 1024 * 1024
        property double resourceIncomingDiskBytes: 50 * 1024 * 1024 * 1024
        property int resourceMinimumFreeSpacePercent: 10
        property int resourceSocketOutputBytes: 256 * 1024
    }

    function commit() {
        settings.unapprovedConnections = unapprovedConnections.value
        settings.outstandingRequests = outstandingRequests.value
        settings.pendingConnectionRequests = pendingConnectionRequests.value
        settings.activeFileTransfers = activeFileTransfers.value
        settings.queuedFileOffers = queuedFileOffers.value
        settings.retainedInterruptedTransfers = retainedInterruptedTransfers.value
        settings.resourceAcceptedFileBytes = acceptedFileGiB.value * 1024 * 1024 * 1024
        settings.resourceUnfinishedIncomingBytes = unfinishedIncomingGiB.value * 1024 * 1024 * 1024
        settings.resourceIncomingDiskBytes = incomingDiskGiB.value * 1024 * 1024 * 1024
        settings.resourceMinimumFreeSpacePercent = minimumFreeSpacePercent.value
        settings.resourceSocketOutputBytes = socketOutputKiB.value * 1024
    }

    ColumnLayout {
        id: content
        width: parent.width
        spacing: 8

        Label {
            text: qsTr("Resource limits")
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("These limits protect your device and connection. Values are restricted to the v3 protocol ceilings.")
        }

        ResourceLimitRow { id: unapprovedConnections; label: qsTr("Unapproved connections"); from: 1; to: 32; value: settings.unapprovedConnections }
        ResourceLimitRow { id: outstandingRequests; label: qsTr("Outstanding requests per connection"); from: 1; to: 256; value: settings.outstandingRequests }
        ResourceLimitRow { id: pendingConnectionRequests; label: qsTr("Pending connection requests"); from: 1; to: 256; value: settings.pendingConnectionRequests }
        ResourceLimitRow { id: activeFileTransfers; label: qsTr("Active file transfers per contact"); from: 1; to: 2; value: settings.activeFileTransfers }
        ResourceLimitRow { id: queuedFileOffers; label: qsTr("Queued file offers per peer"); from: 1; to: 64; value: settings.queuedFileOffers }
        ResourceLimitRow { id: retainedInterruptedTransfers; label: qsTr("Retained interrupted transfers per peer"); from: 1; to: 32; value: settings.retainedInterruptedTransfers }
        ResourceLimitRow { id: acceptedFileGiB; label: qsTr("Maximum accepted file size (GiB)"); from: 1; to: 1024; value: settings.resourceAcceptedFileBytes / (1024 * 1024 * 1024) }
        ResourceLimitRow { id: unfinishedIncomingGiB; label: qsTr("Unfinished incoming data per peer (GiB)"); from: 1; to: 2048; value: settings.resourceUnfinishedIncomingBytes / (1024 * 1024 * 1024) }
        ResourceLimitRow { id: incomingDiskGiB; label: qsTr("Maximum incoming transfer disk usage (GiB)"); from: 1; to: 50; value: settings.resourceIncomingDiskBytes / (1024 * 1024 * 1024) }
        ResourceLimitRow { id: minimumFreeSpacePercent; label: qsTr("Required available disk space (%)"); from: 1; to: 100; value: settings.resourceMinimumFreeSpacePercent }
        ResourceLimitRow { id: socketOutputKiB; label: qsTr("Socket output queue (KiB)"); from: 16; to: 256; value: settings.resourceSocketOutputBytes / 1024 }
    }
}
