
#include "ds/messagesmodel.h"
#include "ds/dsengine.h"
#include "ds/dscert.h"
#include "ds/textpolicy.h"
#include "ds/historyquery.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlRecord>
#include <QUuid>
#include <QSqlDatabase>
#include <algorithm>
#include <tuple>

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace ds::crypto;

namespace ds {
namespace models {

MessagesModel::MessagesModel(QObject &parent)
    : QAbstractListModel(&parent)
{
    auto mgr = DsEngine::instance().getMessageManager();
    auto fmgr = DsEngine::instance().getFileManager();
    connect(mgr, &MessageManager::messageAdded,
            this, &MessagesModel::onMessageAdded);
    connect(mgr, &MessageManager::messageDeleted,
            this, &MessagesModel::onMessageDeleted);
    connect(mgr, &MessageManager::messageReceivedDateChanged,
            this, &MessagesModel::onMessageReceivedDateChanged);
    connect(mgr, &MessageManager::messageStateChanged,
            this, &MessagesModel::onMessageStateChanged);
    connect(fmgr, &FileManager::fileAdded,
            this, &MessagesModel::onFileAdded);
    connect(fmgr, &FileManager::fileDeleted,
            this, &MessagesModel::onFileDeleted);
    connect(fmgr, &FileManager::fileStateChanged,
            this, &MessagesModel::onFileStateChanged);
    connect(DsEngine::instance().getContactManager(),
            &ContactManager::contactDeleted,
            this, &MessagesModel::onContactDeleted);
}

MessagesModel::~MessagesModel()
{
    history_thread_.quit();
    history_thread_.wait();
}

QString MessagesModel::draft() const
{
    return conversation_ ? drafts_.value(conversation_->getId()) : QString{};
}

void MessagesModel::setDraft(const QString &text)
{
    if (!conversation_ || draft() == text.left(65536)) return;
    const auto id = conversation_->getId();
    if (text.isEmpty()) {
        drafts_.remove(id);
        std::erase(draft_order_, id);
    } else {
        // Local drafts stay in memory only. Bound both their count and size.
        if (!drafts_.contains(id)) {
            if (draft_order_.size() >= 128) {
                drafts_.remove(draft_order_.front());
                draft_order_.pop_front();
            }
            draft_order_.push_back(id);
        }
        drafts_.insert(id, text.left(65536));
    }
    emit draftChanged();
}

void MessagesModel::onContactDeleted(const QUuid &uuid)
{
    if (conversation_ && conversation_->getFirstParticipant()
        && conversation_->getFirstParticipant()->getUuid() == uuid) {
        drafts_.remove(conversation_->getId());
        std::erase(draft_order_, conversation_->getId());
        setConversation(nullptr);
    }
}

void MessagesModel::setConversation(Conversation *conversation)
{
    if (conversation == conversation_.get()) return;
    disconnect(policy_connection_);
    conversation_ = conversation ? conversation->shared_from_this() : nullptr;
    if (conversation_ && conversation_->getFirstParticipant()) {
        auto *identity = conversation_->getFirstParticipant()->getIdentity();
        policy_connection_ = connect(identity, &Identity::securityPreferencesChanged, this, [this] {
            if (!rows_.empty()) emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1), {H_CONTENT});
        });
    }
    loadLatest();
    emit draftChanged();
}

void MessagesModel::loadLatest()
{
    ++generation_;
    beginResetModel();
    rows_.clear();
    endResetModel();
    has_older_ = false;
    has_newer_ = false;
    history_error_.clear();
    if (!loading_) requestPage(0);
    emit historyChanged();
}
void MessagesModel::loadOlder() { if (has_older_ && !loading_) requestPage(-1); }
void MessagesModel::loadNewer() { if (has_newer_ && !loading_) requestPage(1); }

int MessagesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant MessagesModel::data(const QModelIndex &ix, int role) const
{
    if (!ix.isValid() || ix.row() < 0 || ix.row() >= rowCount()) {
        return {};
    }

    auto &r = rows_.at(static_cast<size_t>(ix.row()));

    // Lazy loading
    if (!r.loaded()) return {};

    switch(role) {
    case H_TYPE:
        return r.type_;
    case H_ID:
        return r.id;
    case H_FILE:
        return QVariant::fromValue<ds::core::File *>(r.file_.get());
    case H_CONTENT:
        if (r.type_ != MESSAGE) return {};
        if (r.data_->direction == Message::INCOMING && conversation_ && conversation_->getFirstParticipant()
            && !conversation_->getFirstParticipant()->getIdentity()->getAllowUtf8()) {
            if (!core::text_policy::permittedAscii(r.data_->content))
                return tr("Message hidden by your current text security preference.");
        }
        return r.data_->content;
    case H_COMPOSED:
        return (r.type_ == MESSAGE) ? r.data_->composedTime : r.file_->getCreated();
    case H_DIRECTION:
        return (r.type_ == MESSAGE)
                ? static_cast<int>(r.data_->direction)
                : static_cast<int>(r.file_->getDirection());
    case H_RECEIVED:
        return (r.type_ == MESSAGE) ? r.data_->sentReceivedTime : QVariant();
    case H_STATE:
        return (r.type_ == MESSAGE)
                ? static_cast<int>(r.data_->state)
                : static_cast<int>(r.file_->getState());
    case H_STATE_NAME:
        return getStateName(r);
    }

    return {};
}

QHash<int, QByteArray> MessagesModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {H_ID, "messageId"},
        {H_CONTENT, "content"},
        {H_COMPOSED, "composedTime"},
        {H_DIRECTION, "direction"},
        {H_RECEIVED, "receivedTime"},
        {H_STATE, "messageState"},
        {H_TYPE, "type"},
        {H_FILE, "file"},
        {H_STATE_NAME, "stateName"}
    };

    return names;
}

Qt::ItemFlags MessagesModel::flags(const QModelIndex&) const
{
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

void MessagesModel::onMessageAdded(const Message::ptr_t &message)
{
    if (message->getContent().isEmpty() && !message->getData().attachments.empty()) return;
    if (!conversation_ || (conversation_->getId() != message->getConversationId())) {
        return; // Irrelevant
    }

    recordPendingMessage(message);
    Row row(message->getId(), loadData(*message));
    row.sort_created = message->getComposedTime().toString(Qt::ISODateWithMs);
    insertLiveRow(std::move(row));
}

void MessagesModel::onMessageDeleted(const Message::ptr_t &message)
{
    if (!conversation_ || (conversation_->getId() != message->getConversationId())) {
        return; // Irrelevant
    }

    recordPendingDeletion(message->getId(), MESSAGE);
    const int messageId = message->getId();
    int rowid = 0;
    for(auto it = rows_.begin(); it != rows_.end(); ++it, ++rowid) {
        if (it->type_ == MESSAGE && it->id == messageId) {
            beginRemoveRows({}, rowid, rowid);
            rows_.erase(it);
            endRemoveRows();
            return;
        }
    }
}

void MessagesModel::onMessageReceivedDateChanged(const Message::ptr_t &message)
{
    onMessageChanged(message, H_RECEIVED);
}

void MessagesModel::onMessageStateChanged(const Message::ptr_t &message)
{
    onMessageChanged(message, H_STATE);
}

void MessagesModel::onFileAdded(const File::ptr_t &file)
{
    if (file->isThumbnail()) return;
    if (!conversation_ || (conversation_->getId() != file->getConversationId())) {
        return; // Irrelevant
    }

    Row row(file->getId(), file);
    row.sort_created = file->getCreated().toString(Qt::ISODateWithMs);
    insertLiveRow(std::move(row));
}

void MessagesModel::onFileDeleted(const int dbId)
{
    if (!conversation_) {
        return; // Irrelevant
    }

    recordPendingDeletion(dbId, FILE);
    int rowid = 0;
    for(auto it = rows_.begin(); it != rows_.end(); ++it, ++rowid) {
        if (it->type_ == FILE && it->id == dbId) {
            beginRemoveRows({}, rowid, rowid);
            rows_.erase(it);
            endRemoveRows();
            return;
        }
    }
}

void MessagesModel::onFileStateChanged(const File *file)
{
     onFileChanged(file, H_STATE);
}

void MessagesModel::recordPendingMessage(const Message::ptr_t &message)
{
    if (!loading_) return;
    if (pending_messages_.size() >= 500 && !pending_messages_.contains(message->getId())) {
        // A busy reader is obsolete; restart from current persisted state.
        ++generation_;
        return;
    }
    pending_messages_.insert(message->getId(), message);
}
void MessagesModel::recordPendingDeletion(int id, Type type)
{
    if (!loading_) return;
    if (pending_deletions_.size() >= 500) { ++generation_; return; }
    pending_deletions_.insert((quint64(type) << 32) | quint32(id));
}

void MessagesModel::insertLiveRow(Row row)
{
    const auto less = [](const Row &left, const Row &right) {
        return std::tie(left.sort_created, left.type_, left.id) < std::tie(right.sort_created, right.type_, right.id);
    };
    if (!rows_.empty() && ((has_older_ && less(row, rows_.front()))
                          || (has_newer_ && less(rows_.back(), row)))) {
        emit historyChanged();
        return;
    }
    const auto position = std::lower_bound(rows_.begin(), rows_.end(), row, less);
    if (position != rows_.end() && position->type_ == row.type_ && position->id == row.id) return;
    const int index = static_cast<int>(std::distance(rows_.begin(), position));
    beginInsertRows({}, index, index);
    rows_.insert(position, std::move(row));
    endInsertRows();
    trimLiveWindow();
}

void MessagesModel::trimLiveWindow()
{
    constexpr size_t maximum_rows = 500;
    if (rows_.size() <= maximum_rows) return;
    const auto count = static_cast<int>(rows_.size() - maximum_rows);
    beginRemoveRows({}, 0, count - 1);
    rows_.erase(rows_.begin(), rows_.begin() + count);
    endRemoveRows();
    has_older_ = true;
    emit historyChanged();
}

void MessagesModel::requestPage(int direction)
{
    if (!conversation_ || loading_) return;
    if (!history_worker_) {
        history_worker_ = new QObject;
        history_worker_->moveToThread(&history_thread_);
        connect(&history_thread_, &QThread::finished, history_worker_, &QObject::deleteLater);
        history_thread_.start();
    }
    const auto database_path = QSqlDatabase::database().databaseName();
    if (database_path == QStringLiteral(":memory:")) {
        history_error_ = tr("History paging requires a file-backed test profile.");
        emit historyChanged();
        return;
    }
    const auto generation = generation_;
    const int conversation_id = conversation_->getId();
    QString cursor_created;
    int cursor_type = 0;
    int cursor_id = 0;
    if (direction != 0 && !rows_.empty()) {
        const auto &cursor = direction < 0 ? rows_.front() : rows_.back();
        cursor_created = cursor.sort_created;
        cursor_type = cursor.type_;
        cursor_id = cursor.id;
    }
    pending_messages_.clear();
    pending_deletions_.clear();
    loading_ = true;
    history_error_.clear();
    emit historyChanged();
    QMetaObject::invokeMethod(history_worker_, [this, database_path, generation, conversation_id,
                                               direction, cursor_created, cursor_type, cursor_id] {
        rows_t rows;
        QString error;
        bool more = false;
        const auto connection_name = QStringLiteral("history-%1").arg(QUuid::createUuid().toString());
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
            database.setDatabaseName(database_path);
            database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000"));
            if (!database.open()) error = QStringLiteral("Unable to open history database");
            else {
                const auto page = readHistoryPage(database, conversation_id, direction,
                                                  {cursor_created, cursor_type, cursor_id});
                if (!page.valid) error = QStringLiteral("Unable to read history page");
                more = page.more;
                for (const auto &record : page.rows) {
                    Row row(record.cursor.id, static_cast<Type>(record.cursor.type));
                    row.sort_created = record.cursor.created;
                    if (row.type_ == MESSAGE) {
                        row.data_ = make_shared<MessageContent>();
                        row.data_->composedTime = QDateTime::fromString(record.cursor.created, Qt::ISODateWithMs);
                        row.data_->state = static_cast<Message::State>(record.state);
                        row.data_->direction = static_cast<Message::Direction>(record.direction);
                        row.data_->sentReceivedTime = record.received;
                        row.data_->content = record.content;
                    }
                    rows.push_back(std::move(row));
                }
            }
        }
        QSqlDatabase::removeDatabase(connection_name);
        if (direction <= 0) std::reverse(rows.begin(), rows.end());
        QMetaObject::invokeMethod(this, [this, generation, direction, rows = std::move(rows), more, error]() mutable {
            acceptPage(generation, direction, std::move(rows), more, error);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void MessagesModel::acceptPage(quint64 generation, int direction, rows_t rows, bool more, const QString &error)
{
    if (generation != generation_) { loading_ = false; loadLatest(); return; }
    if (!error.isEmpty()) {
        loading_ = false;
        LFLOG_ERROR << "History page retrieval failed";
        history_error_ = tr("Unable to load message history. Try opening the conversation again.");
        emit historyChanged();
        return;
    }
    std::erase_if(rows, [this](const Row &row) {
        return pending_deletions_.contains((quint64(row.type_) << 32) | quint32(row.id));
    });
    for (auto &row : rows) {
        if (row.type_ == MESSAGE && pending_messages_.contains(row.id))
            row.data_ = loadData(*pending_messages_.value(row.id));
    }
    pending_messages_.clear();
    pending_deletions_.clear();
    // Resolve bounded file objects on the owning thread. Message delegate data()
    // never performs database reads. File object ownership remains in core.
    try {
        for (auto &row : rows) if (row.type_ == FILE)
            row.file_ = DsEngine::instance().getFileManager()->getFile(row.id);
    } catch (const std::exception &) {
        loading_ = false;
        history_error_ = tr("Unable to load a file offer in this history page.");
        emit historyChanged();
        return;
    }
    // Preserve live changes that arrived while the read transaction was running.
    std::erase_if(rows, [this](const Row &candidate) {
        return std::any_of(rows_.begin(), rows_.end(), [&candidate](const Row &existing) {
            return existing.type_ == candidate.type_ && existing.id == candidate.id;
        });
    });
    if (direction == 0) {
        // Live arrivals can have peer-authored timestamps older than the fetched
        // window. Merge by the exact SQL tuple order before exposing cursors.
        rows.insert(rows.end(), rows_.begin(), rows_.end());
        std::sort(rows.begin(), rows.end(), [](const Row &left, const Row &right) {
            return std::tie(left.sort_created, left.type_, left.id) < std::tie(right.sort_created, right.type_, right.id);
        });
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    } else if (!rows.empty()) {
        const int first = direction < 0 ? 0 : rowCount();
        beginInsertRows({}, first, first + static_cast<int>(rows.size()) - 1);
        if (direction < 0) rows_.insert(rows_.begin(), rows.begin(), rows.end());
        else rows_.insert(rows_.end(), rows.begin(), rows.end());
        endInsertRows();
    }
    if (direction <= 0) has_older_ = more;
    if (direction >= 0) has_newer_ = direction == 0 ? false : more;
    // Retain a bounded nearby window. Paging in the other direction restores
    // evicted history; this cap never restricts access to persisted messages.
    constexpr size_t maximum_rows = 500;
    if (rows_.size() > maximum_rows) {
        const int count = static_cast<int>(rows_.size() - maximum_rows);
        if (direction < 0) {
            beginRemoveRows({}, rowCount() - count, rowCount() - 1);
            rows_.erase(rows_.end() - count, rows_.end());
            has_newer_ = true;
        } else {
            beginRemoveRows({}, 0, count - 1);
            rows_.erase(rows_.begin(), rows_.begin() + count);
            has_older_ = true;
        }
        endRemoveRows();
    }
    LFLOG_DEBUG << "History page loaded: direction=" << direction << ", retained=" << rows_.size();
    loading_ = false;
    emit historyChanged();
    emit pageLoaded(direction);
}

std::shared_ptr<MessageContent> MessagesModel::loadData(const Message &message) const
{
    auto ptr = make_shared<MessageContent>();

    ptr->state = message.getState();
    ptr->direction = message.getDirection();
    ptr->composedTime = message.getComposedTime();
    ptr->sentReceivedTime = message.getSentReceivedTime();
    ptr->content = message.getContent();

    return ptr;
}

void MessagesModel::onMessageChanged(const Message::ptr_t &message, const int role)
{
    if (!conversation_ || (conversation_->getId() != message->getConversationId())) {
        return; // Irrelevant
    }

    recordPendingMessage(message);
    const int messageId = message->getId();
    int rowid = 0;
    for(auto it = rows_.begin(); it != rows_.end(); ++it, ++rowid) {
        if (it->type_ == MESSAGE && it->id == messageId) {

            if (role == H_STATE) {
                it->data_->state = message->getState();
            } else if (role == H_RECEIVED) {
                it->data_->sentReceivedTime = message->getSentReceivedTime();
            }

            LFLOG_TRACE << "Emitting dataChanged for message " << message->getId()
                        << " for role " << role
                        << " on row " << rowid;

            const auto where = index(rowid);
            if (role == H_STATE) {
                emit dataChanged(where, where, {H_STATE, H_STATE_NAME});
            } else {
                emit dataChanged(where, where, {role});
            }
            return;
        }
    }
}

void MessagesModel::onFileChanged(const File *file, const int role)
{
    if (!conversation_ || (conversation_->getId() != file->getConversationId())) {
        return; // Irrelevant
    }

    const int fileId = file->getId();
    int rowid = 0;
    for(auto it = rows_.begin(); it != rows_.end(); ++it, ++rowid) {
        if (it->type_ == FILE && it->id == fileId) {

            LFLOG_TRACE << "Emitting dataChanged for file " << fileId
                        << " for role " << role
                        << " on row " << rowid;

            const auto where = index(rowid);
            if (role == H_STATE) {
                emit dataChanged(where, where, {H_STATE, H_STATE_NAME});
            } else {
                emit dataChanged(where, where, {role});
            }
            return;
        }
    }
}

QString MessagesModel::getStateName(const MessagesModel::Row &r) const
{
    const std::array<QString, 5> mnames = {tr("Composed"), tr("Queued"), tr("Sent"), tr("Delivered"), tr("Rejected")};
    const std::array<QString, 10> fnames = {tr("Created"), tr("Hashing"), tr("Waiting"), tr("Offered"), tr("Queued"),
                                                  tr("Transferring"), tr("Done"), tr("Failed"), tr("Rejected"),
                                                  tr("Cancelled")};

    if (r.type_ == MESSAGE) {
        if (r.data_->direction == Message::OUTGOING) {
            return mnames.at(static_cast<size_t>(r.data_->state));
        }
        return tr("Received");
    }

    assert(r.file_);

    const auto state = r.file_->getState();
    if (state == File::FS_TRANSFERRING) {
        if (r.file_->getDirection() == File::INCOMING) {
            return tr("Receiving");
        }

        return tr("Sending");
    }

    if (state == File::FS_DONE) {
        if (r.file_->getDirection() == File::INCOMING) {
            return tr("Received");
        }

        return tr("Sent");
    }

    return fnames.at(static_cast<size_t>(state));
}



}} // namespaces
