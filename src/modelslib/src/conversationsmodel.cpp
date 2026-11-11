#include "ds/conversationsmodel.h"
#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/model_util.h"
#include "ds/strategy.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QSqlError>

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace std::string_literals;

namespace ds {
namespace models {

ConversationsModel::ConversationsModel(QObject &parent)
    : QAbstractListModel{&parent}
    , identityManager_{*DsEngine::instance().getIdentityManager()}
    , conversationManager_{*DsEngine::instance().getConversationManager()}
    , contactManager_{*DsEngine::instance().getContactManager()}
{
    connect(&conversationManager_,
            &ConversationManager::conversationAdded,
            this, &ConversationsModel::onConversationAdded);

    connect(&conversationManager_,
            &ConversationManager::conversationDeleted,
            this, &ConversationsModel::onConversationDeleted);

    connect(&conversationManager_,
            &ConversationManager::conversationTouched,
            this, &ConversationsModel::onConversationTouched);

    connect(&contactManager_,
            &ContactManager::contactDeleted,
            this, &ConversationsModel::onContactDeleted);

}

void ConversationsModel::setIdentity(const QUuid &uuid)
{
    try {
    setCurrentRow(-1);
    beginResetModel();

    rows_.clear();
    identity_ = identityManager_.identityFromUuid(uuid);

    if (identity_ != nullptr) {
        LFLOG_TRACE << "Loading conversations for Identiy: "
                    << identity_->getName();
        queryRows(rows_);
    }

    endResetModel();
    emit currentRowChanged();
    contact_ = nullptr;
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select identity conversations: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to load conversations: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    }
}

void ConversationsModel::setContact(Contact *contact)
{
    try {
    LFLOG_TRACE << "Selecting contact " << (contact ? contact->getName() : QStringLiteral("[none]"));
    contact_ = contact;

    if (contact) {

        // Make sure we have a current conversation for that contact
        auto defaultConversation = DsEngine::instance().getConversationManager()->getConversation(contact);

        if (!current_ || (current_->getFirstParticipant()->getId() != contact->getId()) ) {
            // Select the default conversation
            setCurrent(defaultConversation.get());
        }
    }
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select contact conversation: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to load contact conversation: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    }
}

void ConversationsModel::setConversationById(const int id)
{
    try {
        setCurrent(conversationManager_.getConversation(id).get());
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select conversation by id: " << ex.what();
    }
}

void ConversationsModel::setCurrent(Conversation *conversation)
{
    try {
    if (!conversation) { setCurrentRow(-1); return; }
    int row = 0;
    for (const auto &entry : rows_) {
        if (entry.uuid == conversation->getUuid()) { setCurrentRow(row); return; }
        ++row;
    }
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select conversation: " << ex.what();
    }
}


int ConversationsModel::getCurrentRow() const noexcept
{
    return currentRow_;
}

void ConversationsModel::setCurrentRow(int row)
{
    if (row < -1 || row >= static_cast<int>(rows_.size())) return;
    if (row != currentRow_) {
        currentRow_ = row;

        if (row >= 0) {
            auto& r = rows_.at(static_cast<size_t>(currentRow_));

            if (!r.conversation) {
                r.conversation = conversationManager_.getConversation(r.uuid);
            }

            current_ = r.conversation;
        } else {
            current_.reset();
        }

        LFLOG_TRACE << "Emitting: currentRowChanged";
        emit currentRowChanged();
    }
}

Conversation *ConversationsModel::getCurrentConversation() const
{
    return current_.get();
}

void ConversationsModel::onConversationAdded(const Conversation::ptr_t &conversation)
{
    if (!identity_) {
        return;
    }

    if (conversation->getIdentityId() != identity_->getId()) {
        return;
    }

    // Always add at top
    beginInsertRows({}, 0, 0);
    rows_.emplace(rows_.begin(), conversation->getUuid());
    if (currentRow_ >= 0) ++currentRow_;
    endInsertRows();
    emit currentRowChanged();
}

// Move to top of list
void ConversationsModel::onConversationTouched(const Conversation::ptr_t &conversation)
{
    const auto uuid = conversation->getUuid();
    int rowid = 0;
    for(auto it = rows_.begin(); it != rows_.end(); ++it, ++rowid) {
        if (it->uuid == uuid) {
            if (rowid) {
                beginMoveRows({}, rowid, rowid, {}, 0);
                auto moved = std::move(*it);
                rows_.erase(it);
                rows_.push_front(std::move(moved));
                if (currentRow_ == rowid) currentRow_ = 0;
                else if (currentRow_ >= 0 && currentRow_ < rowid) ++currentRow_;
                endMoveRows();
                emit currentRowChanged();
            }
            return;
        }
    }
}

void ConversationsModel::onConversationDeleted(const QUuid &uuid)
{
    int row = 0;
    for (auto it = rows_.begin(); it != rows_.end(); ++it, ++row) {
        if (it->uuid != uuid) continue;
        if (currentRow_ == row) setCurrentRow(-1);
        beginRemoveRows({}, row, row);
        rows_.erase(it);
        if (currentRow_ > row) --currentRow_;
        endRemoveRows();
        emit currentRowChanged();
        return;
    }
}

void ConversationsModel::onContactDeleted(const QUuid &uuid)
{
    if (contact_ && contact_->getUuid() == uuid) {
        contact_ = nullptr;
    }

    setCurrentRow(-1);
    beginResetModel();
    rows_.clear();
    queryRows(rows_);
    endResetModel();
    emit currentRowChanged();
}


int ConversationsModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(rows_.size());
}

QVariant ConversationsModel::data(const QModelIndex &ix, int role) const
{
    if (ix.isValid() && ix.column() == 0 && role == Qt::DisplayRole) {
        auto &r = rows_.at(static_cast<size_t>(ix.row()));

        // Lazy loading
        if (!r.conversation) {
            r.conversation = conversationManager_.getConversation(r.uuid);

            if (r.conversation) {
                assert(r.conversation->getUuid() == r.uuid);
            }
        }

        assert(identity_ != nullptr);
        assert(identity_->getId() == r.conversation->getIdentityId());

        return QVariant::fromValue<ds::core::Conversation *>(r.conversation.get());
    }

    return {};
}

QHash<int, QByteArray> ConversationsModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {Qt::DisplayRole, "conversation"},
    };

    return names;
}

void ConversationsModel::queryRows(ConversationsModel::rows_t &rows)
{
    if (!identity_) {
        return;
    }

    QSqlQuery query;
    query.prepare("SELECT uuid FROM conversation WHERE identity=:identity ORDER BY updated DESC");
    query.bindValue(":identity", identity_->getId());

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add Conversation: %1").arg(
                        query.lastError().text()));
    }

    // Populate
    while(query.next()) {
        rows.emplace_back(query.value(0).toUuid());
    }
}


}} // namespaces
