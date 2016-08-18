#pragma once

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak/EventMonitor.h"
#include "darkspeak/FileInfo.h"

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
        void OnIncomingFile(const darkspeak::FileInfo &file) override;
        void OnFileTransferUpdate(const darkspeak::FileInfo& file) override;
        void OnOtherEvent(const Event &event) override;
        void OnListening(const ListeningInfo &endpoint) override {};
        void OnShutdownComplete(const ShutdownInfo &info) override {};
        void OnAvatarReceived(const AvatarInfo& info) override {};

    private:
        FileTransferModel& parent_;
    };

    enum RoleNames {
        BuddyIdRole = Qt::UserRole,
        BuddyNameRole,
        NameRole,
        SizeRole,
        PercentageRole,
        DirectionRole,
        StatusRole,
        IconRole
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

    Q_PROPERTY(QUrl transferStatusIcon
        READ getTransferStatusIcon
        NOTIFY transferStatusIconChanged)

    static QString GetHumanReadableNumber(std::int64_t num);

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
    Q_INVOKABLE void openFolder(int id);

    Q_INVOKABLE void cancelTransfer(int id);
    Q_INVOKABLE void removeTransfer(int id); // Remove from the list
    Q_INVOKABLE void sendFile(const QString buddyHandle, const QUrl file);

protected:
    QHash<int, QByteArray> roleNames() const;

public slots:
    void addFileToList(darkspeak::FileInfo);
    void updateFileInfo(darkspeak::FileInfo);
    void updateActiveTransfers();
    QUrl getTransferStatusIcon() const;
    void deleteEntryFromList(int index);

signals:
    void activeTransfersChanged(int);
    void newTransfer(darkspeak::FileInfo);
    void fileInfoUpdated(darkspeak::FileInfo);
    void transferStatusIconChanged();
    void deleteEntry(int index);

private:
    darkspeak::FileInfo *GetFile(const boost::uuids::uuid& uuid, int &index);
    QUrl GetIconForFile(const darkspeak::FileInfo& fileInfo) const;

    darkspeak::Api& api_;
    std::shared_ptr<Events> event_listener_;
    std::vector<darkspeak::FileInfo> transfers_;
    int active_transfers_ = 0;
};
