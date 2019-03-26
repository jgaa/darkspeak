
#include <chrono>

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

class HashTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    HashTask(QObject *owner, const File::ptr_t& file)
        : QObject{owner}, file_{file}
    {
    }

    void run() override {
        try {
            QFile file(file_->getPath());
            if (!file.open(QIODevice::ReadOnly)) {
                emit hashed({}, "Failed to open file");
                return;
            }

            crypto_hash_sha256_state state = {};
            crypto_hash_sha256_init(&state);

            std::array<uint8_t, 1024 * 8> buffer;
            while(!file.isOpen()) {
                if (file_->getState() != File::FS_HASHING) {
                    LFLOG_WARN << "File #" << file_->getId()
                               << " changed state during hashing. Aborting!";

                    emit hashed({}, "Aborted");
                    file.close();
                    return;
                }

                const auto bytes_read = file.read(reinterpret_cast<char *>(buffer.data()),
                                                  static_cast<qint64>(buffer.size()));
                if (bytes_read > 0) {
                    crypto_hash_sha256_update(&state, buffer.data(),
                                              static_cast<size_t>(bytes_read));
                } else if (bytes_read == 0) {
                    file.close();
                } else {
                    emit hashed({}, "Read failed");
                    file.close();
                    return;
                }
            }

            QByteArray out;
            out.resize(crypto_hash_sha256_BYTES);
            crypto_hash_sha256_final(&state, reinterpret_cast<uint8_t *>(out.data()));
            emit hashed(out, {});
        } catch(const std::exception& ex) {
            qWarning() << "Caught exception from task: " << ex.what();
            emit hashed({}, ex.what());
        }
    }

    static void schedule(QObject *owner, const File::ptr_t& file) {
        auto task = new HashTask{owner, file};
        QThreadPool::globalInstance()->start(task);
    }

signals:
    void hashed(const QByteArray& hash, const QString& failReason);

private:
    const File::ptr_t file_;
};

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

    if (getState() != FS_OFFERED) {
        return;
    }

    LFLOG_DEBUG << "Accepted file #" << getId() << " " << getPath();

    queueForTransfer();
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
        flushBytesAdded();
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
    updateIf("bytes_transferred", bytes,
             data_->bytesTransferred, this,
             &File::bytesTransferredChanged);
}

void File::addBytesTransferred(const size_t bytes)
{
    bytesAdded_ += static_cast<qlonglong>(bytes);
    if (getState() == FS_TRANSFERRING) {
        if (!nextFlush_) {
            nextFlush_ = make_unique<std::chrono::steady_clock::time_point>(
                        std::chrono::steady_clock::now()
                        + std::chrono::seconds(2));
        } else {
            if (std::chrono::steady_clock::now() >= *nextFlush_) {
                flushBytesAdded();
            }
        }
    } else {
        flushBytesAdded();
    }
}

void File::clearBytesTransferred()
{
    bytesAdded_ = {};
    nextFlush_.reset();
    setBytesTransferred(0);
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

quint32 File::getChannel() const noexcept
{
    return channel_;
}

void File::setChannel(quint32 channel)
{
    channel_ = channel;
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

void File::asynchCalculateHash(File::hash_cb_t callback)
{
    setState(File::FS_HASHING);

    auto self = DsEngine::instance().getFileManager()->getFile(getId());
    auto task = make_unique<HashTask>(this, self);

    // Prevent the file from going out of scope while hashing
    // put a smartpointer to it in the lambda
    connect(task.get(), &HashTask::hashed,
            this, [self=move(self), callback=move(callback)](const QByteArray& hash, const QString& failReason) {
        if (callback) {
            try {
                callback(hash, failReason);
            } catch(const std::exception& ex) {
                LFLOG_ERROR << "Caught exeption from hashing callback: " << ex.what();
            }
        }

        // Make sure the file remains in scope a little longer
        DsEngine::instance().getFileManager()->touch(self);
    }, Qt::QueuedConnection);

    QThreadPool::globalInstance()->start(task.release());
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

void File::flushBytesAdded()
{
    if (bytesAdded_) {
        setBytesTransferred(data_->bytesTransferred + bytesAdded_);
        bytesAdded_ = {};
        nextFlush_.reset();
    }
}

void File::queueForTransfer()
{
    assert(getDirection() == INCOMING);
    assert(getState() == FS_OFFERED || getState() == FS_QUEUED);
    setState(FS_QUEUED);
    if (auto contact = getContact()) {
        // FIXME: shared_from_this() don't work from the file instance
        contact->queueFile(
                    DsEngine::instance().getFileManager()->getFile(getId()));
    }
}

void File::transferComplete()
{
    assert (getState() == FS_TRANSFERRING || getState() == FS_HASHING);

    if (getDirection() == INCOMING) {
        LFLOG_INFO << "File #" << getId()
                   << " at path \"" << getPath()
                   << " was successfulle received from Contact " << getContact()->getName()
                   << " to Identity "
                   << getContact()->getIdentity()->getName();
    } else {
        LFLOG_INFO << "File #" << getId()
                   << " at path \"" << getPath()
                   << " was successfully sent to Contact " << getContact()->getName()
                   << " from Identity "
                   << getContact()->getIdentity()->getName();
    }

    // Make sure we don't go out of scope during the state change
    DsEngine::instance().getFileManager()->touch(
                DsEngine::instance().getFileManager()->getFile(getId()));
    setState(FS_DONE);

    emit transferDone(this, true);
}

void File::transferFailed(const QString &reason, const File::State state)
{
    if (getState() == state) {
        return;
    }

    if (getDirection() == OUTGOING) {
        LFLOG_ERROR << "File #" << getId()
                   << " at path \"" << getPath()
                   << " to Contact " << getContact()->getName()
                   << " from Identity "
                   << getContact()->getIdentity()->getName()
                   << " failed: " << reason;
    } else {
        LFLOG_ERROR << "File #" << getId()
                   << " at path \"" << getPath()
                   << " from Contact " << getContact()->getName()
                   << " to Identity "
                   << getContact()->getIdentity()->getName()
                   << " failed: " << reason;
    }

    // Make sure we don't go out of scope during the state change
    DsEngine::instance().getFileManager()->touch(
                DsEngine::instance().getFileManager()->getFile(getId()));

    setState(state);

    // Tell the other side that we failed
    if (auto contact = getContact()) {
        contact->sendAck("IncomingFile", "Failed", getFileId().toBase64());
    }

    emit transferDone(this, false);
}

void File::validateHash()
{
    assert(getDirection() == INCOMING);
    assert(getState() == FS_TRANSFERRING);
    assert(!data_->hash.isEmpty());

    LFLOG_DEBUG << "Validating hash for file #" << getId();

    setState(FS_HASHING);

    // asynchCalculateHash will keep a shared_ptr to the File until it's done
    asynchCalculateHash([this](const QByteArray& hash, const QString& failReason) {
        if (hash.isEmpty()) {
            LFLOG_DEBUG << "Failed to hash file #" << getId() << ":  " << failReason;
            if (getState() == FS_HASHING) {
                transferFailed(failReason);
            }
        } else if (getState() == FS_HASHING) {
            // Binary compare hashes
            if ((hash.size() == data_->hash.size())
                    && (memcmp(hash.constData(), data_->hash.constData(),
                               static_cast<size_t>(hash.size())) == 0)) {
                transferComplete();
            } else {
                transferFailed("Hash from peer and hash from received file mismatch");
            }
        }
    });
}

}} // namespaces
