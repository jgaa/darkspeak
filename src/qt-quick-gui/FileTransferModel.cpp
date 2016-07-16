
#include <QDesktopServices>

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
    connect(this,
            SIGNAL(newTransfer(darkspeak::EventMonitor::FileInfo)),
            this, SLOT(addFileToList(darkspeak::EventMonitor::FileInfo)));

    connect(this,
            SIGNAL(fileInfoUpdated(darkspeak::EventMonitor::FileInfo)),
            this, SLOT(updateFileInfo(darkspeak::EventMonitor::FileInfo)));

    connect(this,
            SIGNAL(deleteEntry(int)),
            this, SLOT(deleteEntryFromList(int)));

    event_listener_ = make_shared<Events>(*this);
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
        case BuddyNameRole: {
            const auto bid = transfers_[row].buddy_id;
            if (auto buddy = api_.GetBuddy(bid)) {
                const auto ui_name = buddy->GetUiName() + " " + bid;
                return {ui_name.c_str()};
            }
            break;
        }
        case NameRole:
            return {transfers_[row].path.filename().c_str()};
        case SizeRole:
            return GetHumanReadableNumber(transfers_[row].length);
        case PercentageRole:
            return {transfers_[row].PercentageComplete()};
        case DirectionRole:
            return transfers_[row].direction == darkspeak::Direction::INCOMING
                ? RECEIVE : SEND;
        case StatusRole:
            return static_cast<State>(
                static_cast<int>(transfers_[row].state));
        case IconRole:
            return GetIconForFile(transfers_[row]);
    }

    return {};
}
QHash<int, QByteArray> FileTransferModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {BuddyIdRole, "buddy_id"},
        {BuddyNameRole, "buddy_name"},
        {NameRole, "name"},
        {SizeRole, "size"},
        {PercentageRole, "percent"},
        {DirectionRole, "direction"},
        {StatusRole, "status"},
        {IconRole, "icon"}
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

    // The list is sorted by buddy id, with new files at the top.
    int ix = 0, i = 0;
    for(auto it = transfers_.begin(); it != transfers_.end(); ++it, ++i) {
        const auto res = it->buddy_id.compare(fi.buddy_id);
        if (res == 0) {
            // Insert before to make our file on top
            ix = i;
            break;
        }
        if (res < 0) {
            // Insert after, (unless we find a better match).
            ix = i + 1;
        }
    }

    emit beginInsertRows(QModelIndex(), ix, ix);
    transfers_.insert(transfers_.begin() + ix, move(fi));
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
        emit transferStatusIconChanged();
        emit activeTransfersChanged(active_transfers_);
    }
}

void FileTransferModel::updateFileInfo(EventMonitor::FileInfo fi)
{
    int row = 0;
    auto file = GetFile(fi.file_id, row);
    if (file) {
        *file = fi;
        try {
            auto mi = index(row, 0);
            LOG_DEBUG_FN << "Refreshing " << *file << " at index " << row;
            emit dataChanged(mi, mi);
        } WAR_CATCH_NORMAL;
    }

    updateActiveTransfers();
}


EventMonitor::FileInfo* FileTransferModel::GetFile(
    const boost::uuids::uuid& uuid, int& index)
{
    index = 0;
    for(auto& transfer: transfers_) {
        if (transfer.file_id == uuid) {
            return &transfer;
        }
        ++index;
    }

    return nullptr;
}


QUrl FileTransferModel::GetIconForFile(const darkspeak::EventMonitor::FileInfo& fileInfo) const
{
    static const std::array<QUrl, 6> icons = {
        // DownloadBuddyNameRole
        QUrl("qrc:/images/FileDownload.svg"),
        QUrl("qrc:/images/FileDownloadFailed.svg"),
        QUrl("qrc:/images/FileDownloadOk.svg"),
        // Upload
        QUrl("qrc:/imagesFileUpload.svg"),
        QUrl("qrc:/images/FileUploadFailed.svg"),
        QUrl("qrc:/images/FileUploadOk.svg"),
    };

    int index = fileInfo.direction == darkspeak::Direction::INCOMING ? 0 : 3;
    switch(fileInfo.state) {
        case darkspeak::EventMonitor::FileInfo::State::DONE:
            index += 2;
            break;
        case darkspeak::EventMonitor::FileInfo::State::PENDING:
        case darkspeak::EventMonitor::FileInfo::State::TRANSFERRING:
            break;
        case darkspeak::EventMonitor::FileInfo::State::ABORTED:
            index += 1;
        break;
    }

    return icons.at(index);
}

QUrl FileTransferModel::getTransferStatusIcon() const
{
    static const std::array<QUrl, 2> icons = {
        QUrl("qrc:/images/FileTansferActive.svg"),
        QUrl("qrc:/images/FileTansferInactive.svg"),
    };

    const auto& rval = icons.at(active_transfers_ ? 0 : 1);
    LOG_DEBUG_FN << "Sending transfers icon: " << rval.toString().toStdString();
    return rval;
}

// TODO: Add file explorer with file selected for KDE, OS/X and Windows
void FileTransferModel::openFolder(int index)
{
    try {
        const auto& fi = transfers_.at(index);
        auto fullpath = boost::filesystem::canonical(fi.path.parent_path());
        auto url = string("file://") + fullpath.string();
        QDesktopServices::openUrl({url.c_str(), QUrl::TolerantMode});
    } WAR_CATCH_NORMAL;
}

void FileTransferModel::deleteTransfer(int index)
{
    try {
        const auto& fi = transfers_.at(index);

        if ((fi.state == EventMonitor::FileInfo::State::PENDING)
            || (fi.state == EventMonitor::FileInfo::State::TRANSFERRING)) {

            AbortFileTransferData aftd;
            aftd.buddy_id = fi.buddy_id;
            aftd.uuid = fi.file_id;
            aftd.reason = "Aborted by user";
            aftd.delete_this = fi.direction == darkspeak::Direction::INCOMING
                ? fi.path : boost::filesystem::path();
            api_.AbortFileTransfer(aftd);
        } else if (fi.state == EventMonitor::FileInfo::State::DONE) {
            if (boost::filesystem::is_regular(fi.path)) {
                LOG_NOTICE << "Removing file " << log::Esc(fi.path.string());
                boost::filesystem::remove(fi.path);
            }
        }

        emit deleteEntry(index);

    } WAR_CATCH_NORMAL;
}

void FileTransferModel::deleteEntryFromList(int index) {

    try {
        const auto& fi = transfers_.at(index);

        LOG_DEBUG_FN << "Removing file transfer " << fi;

        emit beginRemoveRows(QModelIndex(), index, index);
        transfers_.erase(transfers_.begin() + index);
        emit endRemoveRows();

        updateActiveTransfers();
    } WAR_CATCH_NORMAL;
}

QString FileTransferModel::GetHumanReadableNumber(int64_t num) {
    static const auto peta = 1125899906842624;
    static const auto tera = 1099511627776;
    static const auto giga = 1073741824;
    static const auto mega = 1048576;
    static const auto kilo = 1024;

    std::stringstream result;

    result << fixed <<  setprecision(2);

    if (num >= (peta + (peta / 4))) {
        result << (static_cast<double>(num) / peta) << 'p';
    } if (num >= (tera + (tera / 4))) {
        result << (static_cast<double>(num) / tera) << 't';
    } else if (num >= (giga + (giga / 4))) {
        result << (static_cast<double>(num) / giga) << 'g';
    } else if (num >= (mega + (mega / 4))) {
        result << (static_cast<double>(num) / mega) << 'm';
    } else if (num >= (kilo + (kilo / 4))) {
        result << (static_cast<double>(num) / kilo) << 'k';
    } else {
        result << num << 'b';
    }

    auto str = result.str();
    return {str.c_str()};
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

