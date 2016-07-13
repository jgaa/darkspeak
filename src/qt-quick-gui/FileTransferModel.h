#pragma once

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak/EventMonitor.h"

#include <QtCore>


class FileTransferModel : public QAbstractListModel
{
    Q_OBJECT

    class Events : public darkspeak::EventMonitor
    {
    public:
        Events(FileTransferModel& parent);

        // EventMonitor interface
        bool OnIncomingConnection(const ConnectionInfo &info) override {
            return true;
        }
        bool OnAddNewBuddy(const BuddyInfo &info) override {
            return true;
        }
        void OnNewBuddyAdded(const BuddyInfo &info) override {};
        void OnBuddyDeleted(const DeletedBuddyInfo &info) override {};
        void OnBuddyStateUpdate(const BuddyInfo &info) override {};
        void OnIncomingMessage(const Message &message) override {};
        void OnIncomingFile(const FileInfo &file) override;
        void OnFileTransferUpdate(const FileInfo& file) override;
        void OnOtherEvent(const Event &event) override;
        void OnListening(const ListeningInfo &endpoint) override {};
        void OnShutdownComplete(const ShutdownInfo &info) override {};

    private:
        FileTransferModel& parent_;
    };

    enum RoleNames {
        BuddyIdRole = Qt::UserRole,
        NameRole,
        SizeRole,
        PercentageRole,
        DirectionRole,
        StatusRole
    };

public:
    enum Direction {
        RECEIVE,
        SEND
    };

    enum State {
        PENDING,
        TRANSFERRING,
        DONE,
        ABORTED
    };

    Q_ENUMS(Direction)
    Q_ENUMS(State)

    Q_PROPERTY(int activeTransfers
        READ getActiveTransfers
        NOTIFY activeTransfersChanged)

    // QAbstractItemModel interface
public:
    explicit FileTransferModel(darkspeak::Api& api, QObject *parent = nullptr);
    ~FileTransferModel();
    FileTransferModel(const FileTransferModel&) = delete;
    FileTransferModel(FileTransferModel&&) = delete;
    FileTransferModel& operator = (const FileTransferModel&) = delete;
    FileTransferModel& operator = (FileTransferModel&&) = delete;

    virtual int rowCount(const QModelIndex &parent) const override;
    virtual QVariant data(const QModelIndex &index, int role) const override;

    Q_INVOKABLE int getActiveTransfers();

protected:
    QHash<int, QByteArray> roleNames() const;

public slots:
    void addFileToList(darkspeak::EventMonitor::FileInfo fi);
    void updateFileInfo(darkspeak::EventMonitor::FileInfo fi);
    void updateActiveTransfers();

signals:
    void activeTransfersChanged(int);
    void newTransfer(darkspeak::EventMonitor::FileInfo fi);
    void fileInfoUpdated(darkspeak::EventMonitor::FileInfo fi);

private:
    darkspeak::EventMonitor::FileInfo *GetFile(const boost::uuids::uuid& uuid);

    darkspeak::Api& api_;
    std::shared_ptr<Events> event_listener_;
    std::vector<darkspeak::EventMonitor::FileInfo> transfers_;
    int active_transfers_ = 0;
};
