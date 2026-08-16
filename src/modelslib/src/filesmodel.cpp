
#include <QSqlQuery>
#include <QUrl>
#include <QFileInfo>
#include <QSqlError>

#include <cassert>

#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/manager.h"
#include "ds/filesmodel.h"

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;


namespace ds {
namespace models {

FilesModel::FilesModel(QObject &parent)
    : QAbstractListModel(&parent)
{
    connect(DsEngine::instance().getContactManager(),
            &ContactManager::contactDeleted,
            this, &FilesModel::onContactDeleted);
}

void FilesModel::onContactDeleted(const QUuid &uuid)
{
    if (currentContact_ && currentContact_->getUuid() == uuid) {
        currentConversation_.reset();
        currentContact_.reset();
    }

    beginResetModel();
    rows_.clear();
    queryRows(rows_);
    endResetModel();
}

void FilesModel::setConversation(Conversation *conversation)
{
    try {
    if (!conversation) {
        currentConversation_.reset();
        currentContact_.reset();
        currentIdentity_ = nullptr;
        rows_.clear();
        return;
    }
    currentConversation_ = conversation->shared_from_this();
    currentContact_ = conversation->getFirstParticipant()->shared_from_this();
    currentIdentity_ = currentContact_->getIdentity();

    queryRows(rows_);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select conversation files: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to load files: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    }
}

void FilesModel::setContact(Contact *contact)
{
    try {
    if (!contact) {
        currentConversation_.reset();
        currentContact_.reset();
        currentIdentity_ = nullptr;
        rows_.clear();
        return;
    }
    currentConversation_.reset();
    currentContact_ = contact->shared_from_this();
    currentIdentity_ = currentContact_->getIdentity();

    queryRows(rows_);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select contact files: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to load files: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    }
}

void FilesModel::setIdentity(Identity *identity)
{
    try {
    currentConversation_.reset();
    currentContact_.reset();
    currentIdentity_ = identity;

    queryRows(rows_);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select identity files: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to load files: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    }
}

qlonglong FilesModel::getFileLength(const QString &path) const
{
    const auto url = QUrl::fromUserInput(path);
    const auto localPath = url.isLocalFile() ? url.toLocalFile() : path;
    const QFileInfo fileInfo(localPath);
    if (fileInfo.exists() && fileInfo.isFile()) {
        return fileInfo.size();
    }

    return -1;
}

QString FilesModel::getFileName(const QString &path) const
{
    const auto url = QUrl::fromUserInput(path);
    const auto localPath = url.isLocalFile() ? url.toLocalFile() : path;
    return QFileInfo(localPath).fileName();
}

int FilesModel::rowCount(const QModelIndex &) const
{
    return static_cast<int>(rows_.size());
}

QVariant FilesModel::data(const QModelIndex &ix, int role) const
{
    if (ix.isValid() && ix.column() == 0 && role == Qt::DisplayRole) {
        auto &r = rows_.at(static_cast<size_t>(ix.row()));

        // Lazy loading
        if (!r.file) {
            r.file = DsEngine::instance().getFileManager()->getFile(r.id);
        }

        assert(currentIdentity_ != nullptr);

        return QVariant::fromValue<ds::core::File *>(r.file.get());
    }

    return {};
}

QHash<int, QByteArray> FilesModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {Qt::DisplayRole, "file"},
    };

    return names;
}

void FilesModel::onFileAdded(const File::ptr_t &file)
{
    if (!currentIdentity_) {
        return;
    }

    if (currentConversation_) {
        if (currentConversation_->getId() != file->getConversationId()) {
            return;
        }
    } else if (currentContact_) {
        if (currentContact_->getId() != file->getContactId()) {
            return;
        }
    } else if (currentIdentity_->getId() != file->getIdentityId()) {
        return;
    }

    rows_t r;
    queryRows(r);

    // Assume that the r == rows_, except for one added item
    // Just find the insert point an add it

    const auto key = file->getId();
    int rowid = 0;
    auto current_it = rows_.begin();
    for(auto &row : r) {
        if (row.id == key) {

            beginInsertRows({}, rowid, rowid);

            rows_.emplace(current_it, file->getId());

            endInsertRows();
            break;
        }
        ++rowid;
        ++current_it;
    }
}

void FilesModel::onFileDeleted(const int dbId)
{
    int rowid = 0;
    for(auto it = rows_.begin(); it != rows_.end(); ++it, ++rowid) {
        if (it->id == dbId) {
            beginRemoveRows({}, rowid, rowid);
            rows_.erase(it);
            endRemoveRows();
            break;
        }
    }
}

void FilesModel::queryRows(FilesModel::rows_t &rows)
{
    if (!currentIdentity_) {
        return;
    }

    QString where = currentConversation_
            ? "conversation_id=:key"
            : currentContact_ ? "contact_id=:key"
            : "identity_id=:key";
    QSqlQuery query;
    query.prepare(QStringLiteral("SELECT id FROM file WHERE %1 ORDER BY %2").arg(where, "id"));
    query.bindValue(":key", currentConversation_
                    ? currentConversation_->getId()
                    : currentContact_ ? currentContact_->getId()
                    : currentIdentity_->getId());

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to query files: %1").arg(
                        query.lastError().text()));
    }

    // Populate
    while(query.next()) {
        rows.emplace_back(query.value(0).toInt());
    }
}




}}
