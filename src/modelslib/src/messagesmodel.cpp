
#include "ds/messagesmodel.h"
#include "ds/dsengine.h"
#include "ds/dscert.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlRecord>
#include <QUuid>

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
}

void MessagesModel::setConversation(Conversation *conversation)
{
    if (conversation == conversation_.get()) {
        return;
    }

    conversation_ = conversation ? conversation->shared_from_this() : nullptr;

    beginResetModel();

    rows_.clear();
    queryRows(rows_);

    endResetModel();
}

int MessagesModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return static_cast<int>(rows_.size());
}

QVariant MessagesModel::data(const QModelIndex &ix, int role) const
{
    if (!ix.isValid()) {
        return {};
    }

    auto &r = rows_.at(static_cast<size_t>(ix.row()));

    // Lazy loading
    if (!r.loaded()) {
        load(r);
    }

    switch(role) {
    case H_TYPE:
        return r.type_;
    case H_ID:
        return r.id;
    case H_FILE:
        return QVariant::fromValue<ds::core::File *>(r.file_.get());
    case H_CONTENT:
        return (r.type_ == MESSAGE) ? r.data_->content : QVariant();
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
    if (!conversation_ || (conversation_->getId() != message->getConversationId())) {
        return; // Irrelevant
    }

    // Always add at the end
    const int rowid = static_cast<int>(rows_.size());
    beginInsertRows({}, rowid, rowid);
    rows_.push_back({message->getId(), loadData(*message)});
    endInsertRows();
}

void MessagesModel::onMessageDeleted(const Message::ptr_t &message)
{
    if (!conversation_ || (conversation_->getId() != message->getConversationId())) {
        return; // Irrelevant
    }

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
    if (!conversation_ || (conversation_->getId() != file->getConversationId())) {
        return; // Irrelevant
    }

    // Always add at the end
    const int rowid = static_cast<int>(rows_.size());

    beginInsertRows({}, rowid, rowid);
    rows_.push_back({file->getId(), file});
    endInsertRows();
}

void MessagesModel::onFileDeleted(const int dbId)
{
    if (!conversation_) {
        return; // Irrelevant
    }

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

void MessagesModel::queryRows(MessagesModel::rows_t &rows)
{
    if (!conversation_) {
        return;
    }

    QSqlQuery query;
    //query.prepare("SELECT id FROM message WHERE conversation_id=:cid ORDER BY id");
    query.prepare(
        "SELECT 0 as type, id, composed_time AS created FROM message WHERE conversation_id=:cid "
        "UNION ALL "
        "SELECT 1 as type, id, created_time AS created FROM file WHERE conversation_id=:cid "
        "ORDER BY created ");
    query.bindValue(":cid", conversation_->getId());

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add Conversation: %1").arg(
                        query.lastError().text()));
    }

    enum Fiels { type, id };

    // Populate
    while(query.next()) {
        rows.emplace_back(query.value(id).toInt(),
                          static_cast<Type>(query.value(type).toInt()));
    }

    LFLOG_DEBUG << "Loaded " << rows.size() << " rows with messages and/or files";
}

void MessagesModel::load(MessagesModel::Row &row) const
{
    if (row.type_ == MESSAGE) {
        row.data_ = loadData(row.id);
    } else if (row.type_ == FILE) {
        row.file_ = DsEngine::instance().getFileManager()->getFile(row.id);
    }
}

std::shared_ptr<MessageContent> MessagesModel::loadData(const int id) const
{
    // TODO: Try to get it from the message cache

    // Read from database
    QSqlQuery query;

    enum Fields {
        state, direction, composed_time, received_time, content
    };

    query.prepare("SELECT state, direction, composed_time, received_time, content FROM message where id=:id ");
    query.bindValue(":id", id);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch Message: %1").arg(
                        query.lastError().text()));
    }

    if (!query.next()) {
        throw NotFoundError(QStringLiteral("Message not found!"));
    }

    auto ptr = make_shared<MessageContent>();

    ptr->state = static_cast<Message::State>(query.value(state).toInt());
    ptr->direction = static_cast<Message::Direction>(query.value(direction).toInt());
    ptr->composedTime = query.value(composed_time).toDateTime();
    ptr->sentReceivedTime = query.value(received_time).toDateTime();
    ptr->content = query.value(content).toString();

    return ptr;
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
    static const std::array<QString, 5> mnames = {"Composed", "Queued", "Sent", "Delivered", "Rejected"};
    static const std::array<QString, 10> fnames = {"Created", "Hashing", "Waiting", "Offered", "Queued",
                                                  "Transferring", "Done", "Failed", "Rejected",
                                                  "Cancelled"};

    if (r.type_ == MESSAGE) {
        if (r.data_->direction == Message::OUTGOING) {
            return mnames.at(static_cast<size_t>(r.data_->state));
        }
        return "Received";
    }

    assert(r.file_);

    const auto state = r.file_->getState();
    if (state == File::FS_TRANSFERRING) {
        if (r.file_->getDirection() == File::INCOMING) {
            return "Receiving";
        }

        return "Sending";
    }

    if (state == File::FS_DONE) {
        if (r.file_->getDirection() == File::INCOMING) {
            return "Received";
        }

        return "Sent";
    }

    return fnames.at(static_cast<size_t>(state));
}



}} // namespaces
