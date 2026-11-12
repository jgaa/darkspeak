#include "ds/textpolicy.h"

#include <chrono>

#include "ds/errors.h"
#include "ds/dsengine.h"
#include "ds/update_helper.h"
#include "ds/crypto.h"
#include "ds/file.h"



#include <QFileInfo>
#include <QUrl>
#include <QDesktopServices>
#include <QObject>
//#include <QStringLiteral>

#include "logfault/logfault.h"

using namespace std;

namespace ds {
namespace core {

File::File(QObject &)
    : QObject (nullptr), data_{make_unique<FileData>()}
{
}

File::File(QObject &, std::unique_ptr<FileData> data)
    : QObject (nullptr), data_{std::move(data)}
{
    if (auto *identity = DsEngine::instance().getIdentityManager()->identityFromId(data_->identity))
        connect(identity, &Identity::securityPreferencesChanged, this, &File::nameChanged);
}

void File::cancel()
{
    try {
    if (getState() == FS_FAILED || getState() == FS_REJECTED) {
        return;
    }

    if (getState() == FS_TRANSFERRING) {
        storage_cancel_.cancel();
        if (getDirection() == INCOMING) abortIncoming();
        transferFailed("Cancelled");
        return;
    }

    storage_cancel_.cancel();
    if (getDirection() == INCOMING) abortIncoming();
    setState(FS_CANCELLED);

    LFLOG_DEBUG << "Cancelled file #" << getId();

    if (auto contact = getContact()) {
        if (contact->isOnline()) {
            contact->sendFileCancel(getFileId(), FileCancelReason::UserAction);
        }
    }
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to cancel file #" << getId() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QObject::tr("Failed to cancel file: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to cancel file #" << getId() << ": unknown exception";
    }
}

void File::accept()
{
    try {
    if (getDirection() != INCOMING) {
        return;
    }

    if (getState() != FS_OFFERED) {
        return;
    }

    QString reason;
    auto fileManager = DsEngine::instance().getFileManager();
    if (!fileManager->canAcceptIncoming(*this, reason)) {
        DsEngine::instance().events().append(EventJournal::PolicyRejection, getIdentityId(), getContactId(), EventJournal::Incoming, EventJournal::Rejected);
        DsEngine::instance().reportStatus(
            QObject::tr("Cannot receive %1: %2").arg(getName(), reason),
            DsEngine::StatusLevel::ERROR);
        return;
    }

    LFLOG_DEBUG << "Accepted file #" << getId();

    queueForTransfer();
    getConversation()->touchLastActivity();
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to accept file #" << getId() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QObject::tr("Failed to accept file: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to accept file #" << getId() << ": unknown exception";
    }
}

void File::reject()
{
    try {
    if (getDirection() != INCOMING) {
        return;
    }

    setState(FS_REJECTED);

    LFLOG_DEBUG << "Rejected file #" << getId();

    if (auto contact = getContact()) {
        if (contact->isOnline()) {
            contact->sendFileOfferResponse(getFileId(), FileOfferResponseStatus::Rejected);
        }
    }
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to reject file #" << getId() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QObject::tr("Failed to reject file: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to reject file #" << getId() << ": unknown exception";
    }
}

void File::resume()
{
    try {
    if (getState() != FS_FAILED) {
        return;
    }

    auto contact = getContact();
    if (!contact || !contact->isOnline() || contact->getState() != Contact::ACCEPTED) {
        return;
    }

    if (getDirection() == OUTGOING) {
        // Manual outbound retry re-offers the existing transfer. The receiver
        // validates its durable partial before creating a new channel.
        setState(FS_WAITING);
        contact->queueFile(DsEngine::instance().getFileManager()->getFile(getId()));
        return;
    }

    resumeIncoming(shared_from_this());
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to resume file #" << getId() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QObject::tr("Failed to resume file: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to resume file #" << getId() << ": unknown exception";
    }
}

void File::openInDefaultApplication()
{
    openExternal(shared_from_this(), false);
}
void File::openFolder()
{
    openExternal(shared_from_this(), true);
}
QCoro::Task<void> File::openExternal(File::ptr_t self, bool folder)
{
    if ((getDirection() == INCOMING && getState() != FS_DONE) || storageId().isNull()) co_return;
    auto& storage = DsEngine::instance().getFileManager()->storage();
    auto exported = co_await storage.exportFile(storageNamespace(), storageId(), getName());
    if (!exported) {
        DsEngine::instance().reportStatus(tr("Unable to export the file for an external application."), DsEngine::ERROR);
        co_return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder ? QFileInfo(exported.value()).absolutePath() : exported.value()));
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
        if (state == FS_TRANSFERRING || state == FS_DONE || state == FS_FAILED
            || state == FS_REJECTED || state == FS_CANCELLED) {
            DsEngine::instance().events().append(EventJournal::TransferState, data_->identity, data_->contact,
                getDirection() == INCOMING ? EventJournal::Incoming : EventJournal::Outgoing,
                state == FS_DONE ? EventJournal::Success : state == FS_REJECTED ? EventJournal::Rejected
                    : (state == FS_FAILED || state == FS_CANCELLED) ? EventJournal::Failed : EventJournal::Pending,
                getFileId(), getName());
        }
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

QString File::getDisplayName() const
{
    const auto *identity = DsEngine::instance().getIdentityManager()->identityFromId(data_->identity);
    if (getDirection() == INCOMING && identity && !identity->getAllowUtf8()
        && !text_policy::permittedAscii(data_->name, false))
        return tr("Filename hidden by text security preference");
    return data_->name;
}

QString File::getMimeType() const
{
    return data_->mime_type.isEmpty() ? QStringLiteral("application/octet-stream") : data_->mime_type;
}

void File::setName(const QString &name)
{
    updateIf("name", name, data_->name, this, &File::nameChanged);
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
                        + std::chrono::milliseconds(700));
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

float File::getProgress() const noexcept
{
//    if (getState() == State::FS_DONE) {
//        return 1.0F;
//    }

    if (!getSize()) {
        return 0.0F;
    }
    const auto status = ((static_cast<float>(getBytesTransferred()) / static_cast<float>(getSize())));

    LFLOG_TRACE << "File transfer or file #" << getId() << " is at " << static_cast<int>(status * 100.0F) << "%.";

    return status;
}

dsfm::NamespaceId File::storageNamespace() const { return data_->storage_namespace; }
dsfm::FileId File::storageId() const { return data_->storage_id; }
dsfm::PendingId File::pendingId() const { return data_->pending_id; }
void File::saveStorage() {
    QSqlQuery query;
    query.prepare("UPDATE file SET storage_namespace=:ns,storage_id=:file,pending_id=:pending,mime_type=:mime,path=NULL WHERE id=:id");
    query.bindValue(":ns", data_->storage_namespace.isNull() ? QVariant{} : QVariant(data_->storage_namespace));
    query.bindValue(":file", data_->storage_id.isNull() ? QVariant{} : QVariant(data_->storage_id));
    query.bindValue(":pending", data_->pending_id.isNull() ? QVariant{} : QVariant(data_->pending_id));
    query.bindValue(":mime", data_->mime_type);
    query.bindValue(":id", getId());
    if (!query.exec()) throw Error("Cannot persist managed file references");
}
QCoro::Task<dsfm::Result<quint64>> File::prepareIncoming() {
    auto self = shared_from_this();
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    using dsfm::Error; using dsfm::ErrorCode;
    if (storage_busy_ || writer_) co_return Error{ErrorCode::Busy};
    if (getDirection() != INCOMING || getSize() < 0 || getHash().size() != 32) co_return Error{ErrorCode::InvalidArgument};
    storage_busy_ = true;
    struct Guard { File* file; ~Guard() { file->storage_busy_ = false; } } guard{this};
    storage_cancel_ = {};
    auto& storage = DsEngine::instance().getFileManager()->storage();
    auto ready = co_await storage.ready();
    if (!ready) co_return ready.error();
    if (storage_cancel_.cancelled()) co_return Error{ErrorCode::Cancelled};
    quint64 offset = 0;
    if (!pendingId().isNull()) {
        if (!storage.isDownloadNamespace(storageNamespace())) co_return Error{ErrorCode::InvalidArgument};
        auto opened = co_await storage.manager().openPending(storageNamespace(), pendingId());
        if (!opened) co_return opened.error();
        auto pending = std::move(opened).value();
        if (pending.info.file_id != storageId() || pending.info.content.expected_sha256 != getHash()
            || pending.info.content.expected_size != std::optional<quint64>(quint64(getSize()))) co_return Error{ErrorCode::Corruption};
        writer_ = std::move(pending.writer);
        // Resume only an acknowledged, durable contiguous prefix, never sparse-file length.
        dsfm::PageRequest page;
        do {
            auto ranges = co_await storage.manager().listRanges(storageNamespace(), pendingId(), dsfm::RangeKind::StorageValid, page);
            if (!ranges) { writer_.reset(); co_return ranges.error(); }
            for (const auto range : ranges.value().entries) {
                if (range.offset != offset) { writer_.reset(); co_return Error{ErrorCode::IncompleteContent}; }
                offset += range.length;
            }
            page.cursor = ranges.value().next_cursor;
        } while (!page.cursor.isEmpty());
        offset = qMin(offset, quint64(qMax<qlonglong>(0, getBytesTransferred())));
        offset -= offset % (8 * 1024);
    } else {
        auto directory = storage.root(ManagedStorage::Downloads);
        if (DsEngine::instance().settings().value("includeConversationNameInDownloadPath", false).toBool()) {
            const auto name = QStringLiteral("conversation-%1").arg(getConversationId());
            auto found = co_await storage.manager().lookup(storage.namespaceId(ManagedStorage::Downloads), name);
            if (found) directory = found.value().id;
            else if (found.error().code == ErrorCode::MissingEntry) {
                auto made = co_await storage.manager().mkdir(storage.namespaceId(ManagedStorage::Downloads), directory, name);
                if (!made && made.error().code == ErrorCode::Conflict) {
                    auto existing = co_await storage.manager().lookup(storage.namespaceId(ManagedStorage::Downloads), name);
                    if (!existing) co_return existing.error();
                    directory = existing.value().id;
                } else if (!made) co_return made.error();
                else directory = made.value();
            } else co_return found.error();
        }
        dsfm::CreateOptions options{directory, getName()};
        options.content = {quint64(getSize()), getHash()};
        auto created = co_await storage.manager().createFile(storage.namespaceId(ManagedStorage::Downloads), options);
        if (!created) co_return created.error();
        auto pending = std::move(created).value();
        data_->storage_namespace = storage.namespaceId(ManagedStorage::Downloads);
        data_->storage_id = pending.info.file_id;
        data_->pending_id = pending.info.id;
        writer_ = std::move(pending.writer);
        bool saved = true;
        try { saveStorage(); } catch (...) { saved = false; }
        if (!saved) { co_await writer_->abort(); writer_.reset(); co_return Error{ErrorCode::IoError}; }
    }
    if (storage_cancel_.cancelled()) {
        if (getState() == FS_CANCELLED) co_await abortIncoming();
        co_return Error{ErrorCode::Cancelled};
    }
    setBytesTransferred(qlonglong(offset));
    co_return offset;
}
QCoro::Task<dsfm::Result<qsizetype>> File::writeIncoming(quint64 offset, QByteArray bytes) {
    auto self = shared_from_this();
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    if (!writer_) co_return dsfm::Error{dsfm::ErrorCode::MissingEntry};
    co_return co_await writer_->writeAt(offset, std::move(bytes), storage_cancel_);
}
QCoro::Task<dsfm::Result<void>> File::checkpointIncoming() {
    auto self = shared_from_this();
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    if (!writer_) co_return dsfm::Error{dsfm::ErrorCode::MissingEntry};
    co_return co_await writer_->checkpoint(storage_cancel_);
}
QCoro::Task<dsfm::Result<void>> File::suspendIncoming() {
    auto self = shared_from_this();
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    if (storage_busy_) co_return dsfm::Error{dsfm::ErrorCode::Busy};
    if (!writer_) co_return dsfm::Result<void>{};
    storage_busy_ = true;
    struct Guard { File* file; ~Guard() { file->storage_busy_ = false; } } guard{this};
    auto result = co_await writer_->suspend();
    if (result) writer_.reset();
    co_return result;
}
QCoro::Task<dsfm::Result<void>> File::abortIncoming() {
    auto self = shared_from_this();
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    if (writer_) {
        auto result = co_await writer_->abort();
        if (result) writer_.reset();
        co_return result;
    }
    if (pendingId().isNull()) co_return dsfm::Result<void>{};
    auto& storage = DsEngine::instance().getFileManager()->storage();
    auto ready = co_await storage.ready();
    if (!ready) co_return ready.error();
    co_return co_await storage.manager().abortPending(storageNamespace(), pendingId());
}
QCoro::Task<dsfm::Result<void>> File::validateSourceSnapshot() {
    auto self = shared_from_this();
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    auto& storage = DsEngine::instance().getFileManager()->storage();
    auto ready = co_await storage.ready();
    if (!ready) co_return ready.error();
    auto info = co_await storage.manager().stat(storageNamespace(), storageId());
    if (!info) co_return info.error();
    if (info.value().sha256 != getHash() || info.value().size != quint64(getSize())) co_return dsfm::Error{dsfm::ErrorCode::IntegrityError};
    co_return dsfm::Result<void>{};
}
QCoro::Task<void> File::resumeIncoming(File::ptr_t self) {
    auto offset = co_await prepareIncoming();
    if (!offset) { DsEngine::instance().reportStatus(tr("Unable to reopen the pending transfer."), DsEngine::ERROR); co_return; }
    auto suspended = co_await suspendIncoming();
    if (!suspended) co_return;
    if (auto* contact = getContact(); contact && contact->isOnline()) contact->resumeFile(*this);
}

Contact *File::getContact() const
{
    return DsEngine::instance().getContactManager()->getContact(getContactId()).get();
}

void File::addToDb()
{
    QSqlQuery query;
    query.prepare("INSERT INTO file ("
                  "state, direction, identity_id, conversation_id, contact_id, hash, file_id, name, path, size, file_time, created_time, ack_time, bytes_transferred, storage_namespace, storage_id, pending_id, mime_type"
                  ") VALUES ("
                  ":state, :direction, :identity_id, :conversation_id, :contact_id, :hash, :file_id, :name, :path, :size, :file_time, :created_time, :ack_time, :bytes_transferred, :storage_namespace, :storage_id, :pending_id, :mime_type"
                  ")");

    if (!data_->createdTime.isValid()) {
        data_->createdTime = DsEngine::getSafeNow();
    }

    if (data_->fileId.isEmpty()) {
         data_->fileId = crypto::Crypto::getRandomBytes(16);
    }

    query.bindValue(":state", static_cast<int>(data_->state));
    query.bindValue(":direction", static_cast<int>(data_->direction));
    query.bindValue(":identity_id", data_->identity);
    query.bindValue(":conversation_id", data_->conversation);
    query.bindValue(":contact_id", data_->contact);
    query.bindValue(":hash", data_->hash);
    query.bindValue(":file_id", data_->fileId);
    query.bindValue(":name", data_->name);
    query.bindValue(":path", QVariant{});
    query.bindValue(":storage_namespace", data_->storage_namespace.isNull() ? QVariant{} : QVariant(data_->storage_namespace));
    query.bindValue(":storage_id", data_->storage_id.isNull() ? QVariant{} : QVariant(data_->storage_id));
    query.bindValue(":pending_id", data_->pending_id.isNull() ? QVariant{} : QVariant(data_->pending_id));
    query.bindValue(":mime_type", data_->mime_type);
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

    LFLOG_DEBUG << "Added managed file metadata";
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

void File::importIntoStorage(File::hash_cb_t callback)
{
    setState(FS_HASHING);
    importSource(shared_from_this(), std::move(callback));
}
QCoro::Task<void> File::importSource(File::ptr_t self, hash_cb_t callback) {
    auto& storage = DsEngine::instance().getFileManager()->storage();
    auto imported = co_await storage.importFile(data_->source_path, getName(), storage_cancel_, [this, &storage](dsfm::PendingInfo pending) {
        data_->storage_namespace = storage.namespaceId(ManagedStorage::Private);
        data_->storage_id = pending.file_id;
        data_->pending_id = pending.id;
        try { saveStorage(); return true; } catch (...) { return false; }
    });
    data_->source_path.clear();
    if (!imported) { if (callback) callback({}, tr("Unable to import the selected file into managed storage.")); co_return; }
    data_->storage_namespace = imported.value().namespace_id;
    data_->storage_id = imported.value().file.id;
    data_->pending_id = {};
    data_->mime_type = imported.value().mime_type;
    try {
        saveStorage();
        setSize(qlonglong(imported.value().file.size));
        if (callback) callback(imported.value().file.sha256, {});
    } catch (...) { transferFailed(tr("Unable to save managed file metadata."), FS_FAILED); }
}

QString File::getSelectStatement(const QString &where)
{
    return QStringLiteral("SELECT id, file_id, state, direction, identity_id, conversation_id, contact_id, hash, name, path, size, file_time, created_time, ack_time, bytes_transferred, storage_namespace, storage_id, pending_id, mime_type FROM file WHERE %1")
            .arg(where);
}

File::ptr_t File::load(QObject &parent, const std::function<void (QSqlQuery &)> &prepare)
{
    QSqlQuery query;

    enum Fields {
        id, file_id, state, direction, identity_id, conversation_id, contact_id, hash, name, path, size, file_time, created_time, ack_time, bytes_transferred, storage_namespace, storage_id, pending_id, mime_type
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
    if (auto *identity = DsEngine::instance().getIdentityManager()->identityFromId(ptr->data_->identity))
        connect(identity, &Identity::securityPreferencesChanged, ptr.get(), &File::nameChanged);
    ptr->data_->conversation = query.value(conversation_id).toInt();
    ptr->data_->contact = query.value(contact_id).toInt();
    ptr->data_->hash = query.value(hash).toByteArray();
    ptr->data_->name = query.value(name).toString();
    ptr->data_->storage_namespace = query.value(storage_namespace).toUuid();
    ptr->data_->storage_id = query.value(storage_id).toUuid();
    ptr->data_->pending_id = query.value(pending_id).toUuid();
    ptr->data_->mime_type = query.value(mime_type).toString();
    ptr->data_->size = query.value(size).toLongLong();
    ptr->data_->fileTime = query.value(file_time).toDateTime();
    ptr->data_->createdTime = query.value(created_time).toDateTime();
    ptr->data_->ackTime = query.value(ack_time).toDateTime();
    ptr->data_->bytesTransferred = query.value(bytes_transferred).toLongLong();

    // A live channel cannot survive a process restart. Preserve the durable
    // prefix and expose the transfer as manually resumable instead of leaving
    // a stale TRANSFERRING/HASHING state in the UI and database.
    if (ptr->data_->state == FS_TRANSFERRING || ptr->data_->state == FS_HASHING) {
        ptr->data_->state = FS_FAILED;
        QSqlQuery recovery;
        recovery.prepare("UPDATE file SET state=:state WHERE id=:id");
        recovery.bindValue(":state", static_cast<int>(FS_FAILED));
        recovery.bindValue(":id", ptr->id_);
        if (!recovery.exec()) {
            throw Error(QStringLiteral("Failed to recover interrupted file: %1")
                        .arg(recovery.lastError().text()));
        }
    }

    if (!ptr->pendingId().isNull()) ptr->recoverStorage(ptr);
    return ptr;
}
QCoro::Task<void> File::recoverStorage(File::ptr_t self) {
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    auto& storage = DsEngine::instance().getFileManager()->storage();
    auto ready = co_await storage.ready();
    if (!ready) co_return;
    // The library resolves its durable publication first; then repair the application reference.
    auto info = co_await storage.manager().stat(storageNamespace(), storageId());
    if (!info) co_return; // Still pending, or inconsistent: resume/reconciliation remains explicit.
    if (getDirection() == INCOMING && (info.value().sha256 != getHash() || info.value().size != quint64(getSize()))) co_return;
    data_->pending_id = {};
    try {
        saveStorage();
        setSize(qlonglong(info.value().size));
        setHash(info.value().sha256);
        if (getDirection() == INCOMING) { setBytesTransferred(getSize()); setState(FS_DONE); }
        else if (getState() == FS_FAILED || getState() == FS_HASHING) setState(FS_WAITING);
    } catch (...) { LFLOG_WARN << "Cannot repair application reference to recovered managed file"; }
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
    if (getDirection() == INCOMING && (!pendingId().isNull() || storageId().isNull())) {
        transferFailed(tr("Incoming content has not been published."), FS_FAILED);
        return;
    }

    LFLOG_DEBUG << "Managed file transfer completed";
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

    LFLOG_DEBUG << "Managed transfer failed: " << reason;
    storage_cancel_.cancel();
    if (getDirection() == INCOMING && !storage_busy_) suspendIncoming();

    DsEngine::instance().reportStatus(
        reason,
        state == FS_CANCELLED ? DsEngine::StatusLevel::WARNING
                              : DsEngine::StatusLevel::ERROR);

    // Make sure we don't go out of scope during the state change
    DsEngine::instance().getFileManager()->touch(
                DsEngine::instance().getFileManager()->getFile(getId()));

    setState(state);

    // Tell the other side that we failed
    if (auto contact = getContact()) {
        if (contact->isOnline()) {
            switch(state) {
            case FS_REJECTED:
                contact->sendFileOfferResponse(getFileId(), FileOfferResponseStatus::Rejected);
                break;
            case FS_CANCELLED:
                contact->sendFileCancel(getFileId(), FileCancelReason::UserAction);
                break;
            default:
                break;
            }
        }
    }

    emit transferDone(this, false);
}

void File::validateHash()
{
    if (getDirection() != INCOMING || getState() != FS_TRANSFERRING || !writer_) return;
    setState(FS_HASHING);
    commitIncoming(shared_from_this());
}
QCoro::Task<void> File::commitIncoming(File::ptr_t self) {
    auto operation = DsEngine::instance().getFileManager()->storage().holdOperation();
    storage_busy_ = true;
    auto result = co_await writer_->commit(storage_cancel_);
    if (!result) {
        LFLOG_WARN << "Incoming file commit failed (code=" << int(result.error().code) << ")";
        transferFailed(tr("The received file could not be verified or published."), FS_FAILED);
        storage_busy_ = false;
        co_await suspendIncoming();
        co_return;
    }
    storage_busy_ = false;
    writer_.reset();
    data_->pending_id = {};
    try { saveStorage(); } catch (...) { transferFailed(tr("Unable to save completed transfer metadata."), FS_FAILED); co_return; }
    transferComplete(); // A definite publication wins a simultaneous cancellation.
}

}} // namespaces
