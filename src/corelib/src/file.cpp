
#include "ds/errors.h"
#include "ds/dsengine.h"
#include "ds/update_helper.h"
#include "ds/crypto.h"
#include "ds/file.h"
#include "ds/task.h"

#include <sodium.h>

#include "logfault/logfault.h"

#include <QFile>
#include <QFileInfo>
#include <QThreadPool>
#include <QUrl>

using namespace std;

namespace ds {
namespace core {

File::File(QObject &parent)
    : QObject (&parent), data_{make_unique<FileData>()}
{
}

File::File(QObject &parent, std::unique_ptr<FileData> data)
    : QObject (&parent), data_{std::move(data)}
{
}

void File::cancel()
{
    setState(FS_CANCELLED);
}

void File::accept()
{
    if (getDirection() != INCOMING) {
        return;
    }

    setState(FS_WAITING);
    getContact()->queueFile(shared_from_this());
}

void File::reject()
{
    if (getDirection() != INCOMING) {
        return;
    }
    setState(FS_REJECTED);
}

int File::getId() const noexcept
{
    return id_;
}

QByteArray File::getFileId() const noexcept
{
    return data_->fileId;
}

File::State File::getState() const noexcept
{
    return data_->state;
}

void File::setState(const File::State state)
{
    if (updateIf("state", state, data_->state, this, &File::stateChanged)) {
        DsEngine::instance().getFileManager()->onFileStateChanged(this);
    }
}

File::Direction File::getDirection() const noexcept
{
    return data_->direction;
}

QString File::getName() const noexcept
{
    return data_->name;
}

void File::setName(const QString &name)
{
    updateIf("name", name, data_->name, this, &File::nameChanged);
}

QString File::getPath() const noexcept
{
    return data_->path;
}

void File::setPath(const QString &path)
{
    updateIf("path", path, data_->path, this, &File::pathChanged);
}

QByteArray File::getHash() const noexcept
{
    return data_->hash;
}

QString File::getPrintableHash() const noexcept
{
    return getHash().toBase64();
}

void File::setHash(const QByteArray &hash)
{
    updateIf("hash", hash, data_->hash, this, &File::hashChanged);
}

QDateTime File::getCreated() const noexcept
{
    return data_->createdTime;
}

QDateTime File::getFileTime() const noexcept
{
    return data_->fileTime;
}

QDateTime File::getAckTime() const noexcept
{
    return data_->ackTime;
}

qlonglong File::getSize() const noexcept
{
    return data_->size;
}

void File::setSize(const qlonglong size)
{
    updateIf("size", size, data_->size, this, &File::sizeChanged);
}

qlonglong File::getBytesTransferred() const noexcept
{
    return data_->bytesTransferred;
}

void File::setBytesTransferred(const qlonglong bytes)
{
    updateIf("bytes_transferred", bytes, data_->bytesTransferred, this, &File::bytesTransferredChanged);
}

void File::addBytesTransferred(const qlonglong bytes)
{
    setBytesTransferred(getBytesTransferred() + bytes);
}

void File::setAckTime(const QDateTime &when)
{
    updateIf("ack_time", when, data_->ackTime, this, &File::ackTimeChanged);
}

void File::touchAckTime()
{
    setAckTime(DsEngine::getSafeNow());
}

bool File::isActive() const noexcept
{
    auto state = getState();
    return state == FS_WAITING || state == FS_TRANSFERRING;
}

int File::getConversationId() const noexcept
{
    return  data_->conversation;
}

int File::getContactId() const noexcept
{
    return data_->contact;
}

int File::getIdentityId() const noexcept
{
    return data_->identity;
}

Conversation *File::getConversation() const
{
    return DsEngine::instance().getConversationManager()->getConversation(getConversationId()).get();
}

Contact *File::getContact() const
{
    return DsEngine::instance().getContactManager()->getContact(getContactId()).get();
}

void File::addToDb()
{
    QSqlQuery query;
    query.prepare("INSERT INTO file ("
                  "state, direction, identity_id, conversation_id, contact_id, hash, file_id, name, path, size, file_time, created_time, ack_time, bytes_transferred"
                  ") VALUES ("
                  ":state, :direction, :identity_id, :conversation_id, :contact_id, :hash, :file_id, :name, :path, :size, :file_time, :created_time, :ack_time, :bytes_transferred"
                  ")");

    if (!data_->createdTime.isValid()) {
        data_->createdTime = DsEngine::getSafeNow();
    }

    if (data_->direction == File::OUTGOING) {
        QFile file{data_->path};
        if (data_->size == 0) {
            data_->size = file.size();
        }

        if (!data_->fileTime.isValid()) {
            QFileInfo fi{file};
            data_->fileTime = fi.lastModified();
        }
    }

    if (data_->fileId.isEmpty()) {
         data_->fileId = crypto::Crypto::generateId();
    }

    query.bindValue(":state", static_cast<int>(data_->state));
    query.bindValue(":direction", static_cast<int>(data_->direction));
    query.bindValue(":identity_id", data_->identity);
    query.bindValue(":conversation_id", data_->conversation);
    query.bindValue(":contact_id", data_->contact);
    query.bindValue(":hash", data_->hash);
    query.bindValue(":file_id", data_->fileId);
    query.bindValue(":name", data_->name);
    query.bindValue(":path", data_->path);
    query.bindValue(":size", data_->size);
    query.bindValue(":file_time", data_->fileTime);
    query.bindValue(":created_time", data_->createdTime);
    query.bindValue(":ack_time", data_->ackTime);
    query.bindValue(":bytes_transferred", data_->bytesTransferred);
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to save File: %1").arg(
                        query.lastError().text()));
    }

    id_ = query.lastInsertId().toInt();

    LFLOG_INFO << "Added File \"" << getName()
               << "\" with hash " << getHash().toHex()
               << " to the database with id " << id_;
}

void File::deleteFromDb()
{
    if (id_ > 0) {
        QSqlQuery query;
        query.prepare("DELETE FROM file WHERE id=:id");
        query.bindValue(":id", id_);
        if(!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete file: %1").arg(
                            query.lastError().text()));
        }
    }
}

File::ptr_t File::load(QObject &parent, const int dbId)
{
    return load(parent, [dbId](QSqlQuery& query) {
        query.prepare(getSelectStatement("id=:id"));
        query.bindValue(":id", dbId);
    });
}

File::ptr_t File::load(QObject &parent, int conversation, const QByteArray &hash)
{
    return load(parent, [conversation, &hash](QSqlQuery& query) {
        query.prepare(getSelectStatement("hash=:hash AND conversation_id=:cid"));
        query.bindValue(":hash", hash);
        query.bindValue(":cid", conversation);
    });
}

void File::asynchCalculateHash(const File::ptr_t& self)
{
    self->setState(File::FS_HASHING);

    auto task = new Task([self]() {
        QFile file(self->getPath());
        if (!file.open(QIODevice::ReadOnly)) {
            emit self->hashCalculationFailed("Failed to open file");
            return;
        }

        crypto_hash_sha256_state state = {};
        crypto_hash_sha256_init(&state);

        std::array<uint8_t, 1024 * 8> buffer;
        while(!file.isOpen()) {

            const auto bytes_read = file.read(reinterpret_cast<char *>(buffer.data()),
                                              static_cast<qint64>(buffer.size()));
            if (bytes_read > 0) {
                crypto_hash_sha256_update(&state, buffer.data(),
                                          static_cast<size_t>(bytes_read));
            } else if (bytes_read == 0) {
                file.close();
            } else {
                emit self->hashCalculationFailed("Read failed");
                file.close();
                return;
            }
        }

        QByteArray out;
        out.resize(crypto_hash_sha256_BYTES);
        crypto_hash_sha256_final(&state, reinterpret_cast<uint8_t *>(out.data()));
        emit self->hashCalculated(out);
    });

     QThreadPool::globalInstance()->start(task);
}

QString File::getSelectStatement(const QString &where)
{
    return QStringLiteral("SELECT id, file_id, state, direction, identity_id, conversation_id, contact_id, hash, name, path, size, file_time, created_time, ack_time, bytes_transferred FROM file WHERE %1")
            .arg(where);
}

File::ptr_t File::load(QObject &parent, const std::function<void (QSqlQuery &)> &prepare)
{
    QSqlQuery query;

    enum Fields {
        id, file_id, state, direction, identity_id, conversation_id, contact_id, hash, name, path, size, file_time, created_time, ack_time, bytes_transferred
    };

    prepare(query);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch file: %1").arg(
                        query.lastError().text()));
    }

    if (!query.next()) {
        throw NotFoundError(QStringLiteral("file not found!"));
    }

    auto ptr = make_shared<File>(parent);
    ptr->id_ = query.value(id).toInt();
    ptr->data_->fileId = query.value(file_id).toByteArray();
    ptr->data_->state = static_cast<State>(query.value(state).toInt());
    ptr->data_->direction = static_cast<Direction>(query.value(direction).toInt());
    ptr->data_->identity = query.value(identity_id).toInt();
    ptr->data_->conversation = query.value(conversation_id).toInt();
    ptr->data_->contact = query.value(contact_id).toInt();
    ptr->data_->hash = query.value(hash).toByteArray();
    ptr->data_->name = query.value(name).toString();
    ptr->data_->path = query.value(path).toString();
    ptr->data_->size = query.value(size).toLongLong();
    ptr->data_->fileTime = query.value(file_time).toDateTime();
    ptr->data_->createdTime = query.value(created_time).toDateTime();
    ptr->data_->ackTime = query.value(ack_time).toDateTime();
    ptr->data_->bytesTransferred = query.value(bytes_transferred).toLongLong();

    return ptr;
}



}}
