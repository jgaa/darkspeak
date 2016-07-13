
#include "log/WarLog.h"

#include "darkspeak/EventMonitor.h"

#include "FileTransferModel.h"
#include "ChatMessagesModel.h"
#include "ContactData.h"
#include "darkspeak-gui.h"


using namespace std;
using namespace darkspeak;
using namespace war;


FileTransferModel::FileTransferModel(Api& api, QObject *parent)
: QAbstractListModel(parent), api_{api}
{
    event_listener_ = make_shared<Events>(*this);

    connect(this,
            SIGNAL(newTransfer(std::string)),
            this, SLOT(addFileToList(std::string)));

    connect(this,
            SIGNAL(fileInfoUpdated(std::string)),
            this, SLOT(updateFileInfo(std::string)));

    api_.SetMonitor(event_listener_);
}

int FileTransferModel::rowCount(const QModelIndex& parent) const
{
    return static_cast<int>(transfers_.size());
}



QVariant FileTransferModel::data(const QModelIndex& index, int role) const
{
    const auto row = index.row();
        if(row < 0 || row >= static_cast<decltype(row)>(transfers_.size())) {
        return {};
    }

    switch(role) {
        case BuddyIdRole:
            return {transfers_[row].buddy_id.c_str()};
        case NameRole:
            return {transfers_[row].name.c_str()};
        case SizeRole:
            return {static_cast<qint64>(transfers_[row].length)};
        case PercentageRole:
            return {transfers_[row].PercentageComplete()};
        case DirectionRole:
            return transfers_[row].direction == darkspeak::Direction::INCOMING
                ? RECEIVE : SEND;
        case StatusRole:
            return static_cast<State>(
                static_cast<int>(transfers_[row].state));
    }

    return {};
}

QHash<int, QByteArray> FileTransferModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {BuddyIdRole, "buddy_id"},
        {NameRole, "name"},
        {SizeRole, "size"},
        {PercentageRole, "percent"},
        {DirectionRole, "direction"},
        {StatusRole, "status"}
    };

    return names;
}

int FileTransferModel::getActiveTransfers()
{
    return active_transfers_;
}

void FileTransferModel::addFileToList(EventMonitor::FileInfo fi)
{
    LOG_DEBUG_FN << "Adding file transfer " << fi;

    emit beginInsertRows(QModelIndex(), transfers_.size(), transfers_.size());
    transfers_.push_back(move(fi));
    emit endInsertRows();

    updateActiveTransfers();
}

void FileTransferModel::updateActiveTransfers()
{
    int count = 0;
    for(const auto& transfer: transfers_) {
        if (transfer.IsActive()) {
            ++count;
        }
    }

    if (count != active_transfers_) {
        active_transfers_ = count;
        emit activeTransfersChanged(active_transfers_);
    }
}

void FileTransferModel::updateFileInfo(EventMonitor::FileInfo fi)
{
    auto file = GetFile(fi.file_id);
    if (file) {
        *file = fi;
        return;
    }
}


EventMonitor::FileInfo* FileTransferModel::GetFile(const boost::uuids::uuid& uuid)
{
    for(auto& transfer: transfers_) {
        if (transfer.file_id == uuid) {
            return &transfer;
        }
    }

    return nullptr;
}


//////////////// Events ///////////////

FileTransferModel::Events::Events(FileTransferModel& parent)
: parent_{parent}
{
}

FileTransferModel::~FileTransferModel() {

}


void FileTransferModel::Events::OnIncomingFile(const EventMonitor::FileInfo& file)
{
    emit parent_.newTransfer(file);
}


void FileTransferModel::Events::OnFileTransferUpdate(const EventMonitor::FileInfo& file)
{
    emit parent_.fileInfoUpdated(file);
}


void FileTransferModel::Events::OnOtherEvent(const EventMonitor::Event& event)
{

}

