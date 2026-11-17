
#include <QFileInfo>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QCryptographicHash>

#include "include/ds/filemanager.h"
#include "ds/dsengine.h"
#include "ds/resourcelimits.h"
#include "ds/mediapreview.h"
#include "ds/crypto.h"
#include "ds/mimepreferences.h"

#include "logfault/logfault.h"


using namespace std;

namespace ds {
namespace core {

FileManager::FileManager(QObject &parent, QSettings &settings)
    : QObject{&parent}, settings_{settings}, storage_{settings, this}
{
    connect(&storage_, &ManagedStorage::storageError, this, [] {
        DsEngine::instance().reportStatus(tr("File or media storage is unavailable. Check storage permissions and limits."), DsEngine::ERROR);
    });
}

File::ptr_t FileManager::getFile(const int dbId)
{
    auto file = registry_.fetch(dbId);

    if (!file) {
        file = File::load(*this, dbId);
        registry_.add(dbId, file);
    }

    touch(file);
    return file;
}

File::ptr_t FileManager::getFile(const QByteArray &hash, Conversation &conversation)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM file WHERE hash=:hash AND conversation_id=:cid");
    query.bindValue(":hash", hash);
    query.bindValue(":cid", conversation.getId());
    query.exec();
    if (query.next()) {
        return getFile(query.value(0).toInt());
    }

    return {};
}

File::ptr_t FileManager::getFileFromId(const QByteArray &fileId, Conversation &conversation)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM file WHERE file_id=:fid AND conversation_id=:cid");
    query.bindValue(":fid", fileId);
    query.bindValue(":cid", conversation.getId());
    query.exec();

    if (query.next()) {
        return getFile(query.value(0).toInt());
    }

    return {};
}

File::ptr_t FileManager::getFileFromId(const QByteArray &fileId, const File::Direction direction)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM file WHERE file_id=:fid AND direction=:direction");
    query.bindValue(":fid", fileId);
    query.bindValue(":direction", static_cast<int>(direction));
    query.exec();

    if (query.next()) {
        return getFile(query.value(0).toInt());
    }

    return {};
}

File::ptr_t FileManager::getFileFromId(const QByteArray &fileId, const Contact &contact)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM file WHERE file_id=:fid AND contact_id=:cid AND identity_id=:identity LIMIT 2");
    query.bindValue(":fid", fileId);
    query.bindValue(":cid", contact.getId());
    query.bindValue(":identity", contact.getIdentity()->getId());
    query.exec();

    if (query.next()) {
        const auto id = query.value(0).toInt();
        if (query.next()) return {};
        return getFile(id);
    }

    return {};
}

File::ptr_t FileManager::getFileFromId(const QByteArray& file_id, const Contact& contact, File::Direction direction)
{
    return getFileFromPeer(file_id, contact.getIdentity()->getUuid(),
                           contact.getCert()->getHash().toByteArray(), direction);
}

File::ptr_t FileManager::getFileFromPeer(const QByteArray& file_id, QUuid identity,
                                       const QByteArray& peer_hash, File::Direction direction)
{
    if (file_id.size() != 16 || identity.isNull() || peer_hash.isEmpty()) return {};
    QSqlQuery query;
    query.prepare("SELECT f.id FROM file f JOIN contact c ON c.id=f.contact_id "
                  "JOIN identity i ON i.id=f.identity_id WHERE f.file_id=:file "
                  "AND f.direction=:direction AND c.identity=i.id AND i.uuid=:identity AND c.hash=:peer LIMIT 2");
    query.bindValue(":file", file_id);
    query.bindValue(":direction", int(direction));
    query.bindValue(":identity", identity);
    query.bindValue(":peer", peer_hash);
    if (!query.exec()) throw Error("Cannot resolve scoped transfer");
    if (!query.next()) return {};
    const auto id = query.value(0).toInt();
    // Ambiguous local state must never select an arbitrary authority.
    if (query.next()) return {};
    return getFile(id);
}

File::ptr_t FileManager::addFile(std::unique_ptr<FileData> data, std::optional<dsfm::TransferAttempt> attempt)
{
    auto file = make_shared<File>(*this, move(data));
    if (attempt && !file->setCheckpointAttempt(*attempt)) throw Error("Invalid initial checkpoint attempt");
    file->addToDb();
    registry_.add(file->getId(), file);
    touch(file);
    DsEngine::instance().events().append(EventJournal::FileOffer, file->getIdentityId(),
        file->getContactId(), file->getDirection() == File::INCOMING ? EventJournal::Incoming : EventJournal::Outgoing,
        EventJournal::Pending, file->getFileId(), file->getName());
    emit fileAdded(file);

    if (file->getDirection() == File::OUTGOING
            && file->getState() == File::FS_CREATED) {
        importOutgoingFile(file);
    }

    return file;
}

dsfm::Result<File::ptr_t> FileManager::addCatalogTransfer(Conversation& conversation, std::shared_ptr<dsvfs::CatalogExport> catalog) {
    auto contact = conversation.getParticipant();
    if (!contact || !catalog || !catalog->isAuthorized()
        || catalog->identity() != conversation.getIdentity()->getUuid()
        || catalog->authorizationScope() != contact->getUuid()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
    const auto mime = QStringLiteral("application/vnd.darkspeak.vfs-catalog+jsonl");
    if ((catalog->requiresUtf8() && !contact->canSendText(QString::fromUtf8("é"))) || !contact->canSendFileType(mime,"catalog.jsonl")) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
    auto data = std::make_unique<FileData>();
    data->state = File::FS_WAITING; data->identity = conversation.getIdentity()->getId();
    data->contact = contact->getId(); data->conversation = conversation.getId();
    data->name = "catalog.jsonl"; data->mime_type = mime; data->hash = catalog->sha256(); data->size = qlonglong(catalog->size());
    auto file = addFile(std::move(data));
    auto bound = file->setCatalogSource(std::move(catalog));
    if (!bound) { file->deleteFromDb(); return bound.error(); }
    contact->queueFile(file); return file;
}

QString FileManager::transferName(QString logical_name) {
    // Catalog labels preserve Linux names. Transfer suggestions must also fit
    // the existing wire and managed-download component limits.
    auto normalized = dsfm::normalizeName(logical_name); if (normalized) return normalized.value();
    if (logical_name.size() > 3*dsfm::max_encoded_linux_name_bytes) return "download";
    const auto bytes = logical_name.toUtf8(); QByteArray encoded; constexpr char hex[] = "0123456789ABCDEF";
    for (auto ch : bytes) { const auto byte = quint8(ch); encoded.append('%'); encoded.append(hex[byte>>4]); encoded.append(hex[byte&15]); }
    if (encoded.size() > 255) encoded = encoded.first(237)+'-'+QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().first(16);
    if (encoded.isEmpty()) encoded = "download";
    return QString::fromLatin1(encoded);
}

QCoro::Task<dsfm::Result<File::ptr_t>> FileManager::addVfsTransfer(Conversation& conversation,dsvfs::Vfs& vfs,
        dsvfs::NodeId node,dsvfs::Request request,QString mime_type) {
    auto contact = conversation.getParticipant(); QPointer<Identity> identity = conversation.getIdentity(); const auto conversation_id = conversation.getId();
    if (!identity || !contact || request.scope != contact->getUuid() || !contact->canSendFiles()) co_return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
    if (mime_type == "*" || !validIdentityMimePreferences({mime_type})) co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    auto prepared = co_await vfs.prepareRead(identity->getUuid(),node,{},request); if (!prepared) co_return prepared.error();
    const auto transfer_name = transferName(prepared.value().info.name);
    if (!identity || !request.current() || !prepared.value().current || !prepared.value().current()
        || !contact->canSendFileType(mime_type,transfer_name)) { co_await prepared.value().reader->close(); co_return dsfm::Error{dsfm::ErrorCode::PermissionDenied}; }
    struct Source {
        QPointer<dsvfs::Vfs> vfs; QUuid identity; dsvfs::NodeId node; dsvfs::Request request;
        std::optional<dsvfs::PreparedRead> first; QByteArray sha256,lease; quint64 size=0;
        std::function<bool()> current;
    };
    auto source = std::make_shared<Source>(); source->vfs = &vfs; source->identity = identity->getUuid(); source->node = node; source->request = request;
    source->sha256 = prepared.value().info.sha256; source->size = prepared.value().info.size; source->lease = prepared.value().resume_lease;
    source->current = prepared.value().current; source->first.emplace(std::move(prepared).value());
    auto current = [source] { return source->vfs && source->request.current() && source->current && source->current(); };
    auto factory = [source](dsfm::ReadPreparationOptions options) -> QCoro::Task<dsfm::Result<dsfm::PreparedRead>> {
        if (options.cancel.cancelled()) co_return dsfm::Error{dsfm::ErrorCode::Cancelled};
        if (!source->vfs || !source->request.current()) co_return dsfm::Error{dsfm::ErrorCode::MissingEntry};
        if (!options.expected_sha256.isEmpty() && options.expected_sha256 != source->sha256) co_return dsfm::Error{dsfm::ErrorCode::IntegrityError};
        options.expected_sha256 = source->sha256;
        dsvfs::PreparedRead selected;
        if (source->first) { selected = std::move(*source->first); source->first.reset(); }
        else {
            if (options.resume_lease.isEmpty()) options.resume_lease = source->lease;
            auto read = co_await source->vfs->prepareRead(source->identity,source->node,options,source->request); if (!read) co_return read.error(); selected = std::move(read).value();
        }
        source->current = selected.current;
        if (selected.info.size != source->size || selected.info.sha256 != source->sha256) { co_await selected.reader->close(); co_return dsfm::Error{dsfm::ErrorCode::ExternalModification}; }
        dsfm::FileInfo info; info.name = selected.info.name; info.type = dsfm::EntryType::File; info.size = selected.info.size; info.sha256 = selected.info.sha256;
        info.hash_state = selected.info.hash_state; info.generation = selected.info.revision; info.modified = selected.info.modified;
        co_return dsfm::PreparedRead{info,std::move(selected.reader),selected.consistency,selected.resume_lease};
    };
    auto data = std::make_unique<FileData>(); data->state = File::FS_WAITING; data->identity = identity->getId(); data->contact = contact->getId(); data->conversation = conversation_id;
    data->name = transfer_name; data->hash = source->sha256; data->size = qlonglong(source->size); data->mime_type = mime_type;
    auto file = addFile(std::move(data)); auto bound = file->setPreparedSource(source->identity,request.scope,current,factory);
    if (!bound) { file->deleteFromDb(); co_return bound.error(); }
    contact->queueFile(file); co_return file;
}

void FileManager::receivedFileOffer(Conversation& conversation, const PeerFileOffer &offer)
{
    if (!offer.thumbnail_for.isEmpty()) {
        auto primary = getFileFromId(offer.thumbnail_for, *conversation.getFirstParticipant(), File::INCOMING);
        const auto* identity = conversation.getIdentity();
        const auto allowed = primary && primary->getConversationId() == conversation.getId() && !primary->isThumbnail() && primary->getDirection() == File::INCOMING
            && primary->getState() != File::FS_REJECTED && primary->getState() != File::FS_CANCELLED
            && ((primary->getMimeType().startsWith("image/") && identity->getPreviewImages())
                || (media_preview::isVideoCandidate(primary->getMimeType()) && identity->getPreviewVideo()));
        if (!allowed || offer.type != "image/jpeg" || offer.size <= 0
            || offer.size > media_preview::max_encoded_bytes || offer.manifest_id.size() != 32) {
            offer.peer->sendFileOfferResponse(offer.fileId, FileOfferResponseStatus::Rejected);
            return;
        }
    }

    // Check if we have the file.
    try {
        if (auto file = getFileFromId(offer.fileId, *conversation.getFirstParticipant(), File::INCOMING)) {
            if (file->getConversationId() != conversation.getId() || file->getSize() != offer.size || file->getHash() != offer.sha512 || file->getName() != offer.name
                || file->getMimeType() != offer.type || file->dataThumbnailFor() != offer.thumbnail_for) {
                recordPeerEvent(*offer.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
                offer.peer->sendFileOfferResponse(offer.fileId, FileOfferResponseStatus::Rejected);
                return;
            }
            if (file->checkpointInterval() || offer.checkpoint_attempt) {
                if (file->getState() == File::FS_DONE && offer.checkpoint_attempt && file->checkpointInterval() &&
                    file->checkpointInterval() == offer.checkpoint_attempt->interval) {
                    // This offer only triggers EOF recovery. Its nonce cannot
                    // replace the published transfer's attempt or byte prefix.
                    conversation.getFirstParticipant()->recoverCompletedFile(*file);
                    return;
                }
                // Repeated initial offers cannot downgrade or reset a running,
                // paused or completed checkpoint transfer. Recovery uses resume.
                if (!offer.checkpoint_attempt || !file->checkpointInterval() ||
                    file->checkpointInterval() != offer.checkpoint_attempt->interval ||
                    file->getState() == File::FS_REJECTED || file->getState() == File::FS_CANCELLED ||
                    file->failureReason() != int(dsfm::TransferFailureReason::None))
                    offer.peer->sendFileOfferResponse(offer.fileId, FileOfferResponseStatus::Rejected);
                else if (file->isResumable() && !file->storageId().isNull())
                    conversation.getFirstParticipant()->recoverPublishedFile(*file);
                else if (file->getState() == File::FS_HASHING)
                    conversation.getFirstParticipant()->deferCompletionHint(*file);
                // A valid repeated offer is only a hint, even when its nonce
                // differs after a resume. It cannot replace the live attempt,
                // rewind bytes or accept an original while its preview recovers.
                return;
            }
            if (file->getState() == File::FS_REJECTED) {
                offer.peer->sendFileOfferResponse(offer.fileId, FileOfferResponseStatus::Rejected);
            } else if (file->getState() == File::FS_DONE) {
            } else if (file->getState() == File::FS_QUEUED) {
                file->queueForTransfer();
            } else {
                file->setState(File::FS_OFFERED);
            }

            return;
        }
    } catch(const NotFoundError&) {
        ; // It's a new offer
    }

    auto data = make_unique<FileData>();

    const auto limits = ResourceLimits::fromSettings(settings_);
    const auto maxBytes = static_cast<qint64>(limits.acceptedFileBytes);
    if (offer.fileId.size() != 16 || offer.size < 0 || offer.sha512.size() != 32 || !dsfm::normalizeName(offer.name)
            || maxBytes == 0 || offer.size > maxBytes) {
        recordPeerEvent(*offer.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        LFLOG_WARN << "Rejecting file offer: violates local safety policy";
        DsEngine::instance().reportStatus(
            QObject::tr("Rejected file offer due to local safety limits"),
            DsEngine::StatusLevel::WARNING);

        offer.peer->sendFileOfferResponse(offer.fileId, FileOfferResponseStatus::Rejected);
        return;
    }

    QSqlQuery pending;
    pending.prepare("SELECT COUNT(*), COALESCE(SUM(size), 0) FROM file "
                    "WHERE contact_id=:contact AND direction=:direction "
                    "AND state IN (:offered, :queued, :transferring, :hashing)");
    pending.bindValue(":contact", conversation.getFirstParticipant()->getId());
    pending.bindValue(":direction", static_cast<int>(File::INCOMING));
    pending.bindValue(":offered", static_cast<int>(File::FS_OFFERED));
    pending.bindValue(":queued", static_cast<int>(File::FS_QUEUED));
    pending.bindValue(":transferring", static_cast<int>(File::FS_TRANSFERRING));
    pending.bindValue(":hashing", static_cast<int>(File::FS_HASHING));
    if (!pending.exec() || !pending.next()) {
        throw Error("Failed to check pending incoming file quota");
    }
    const auto pendingCount = pending.value(0).toLongLong();
    const auto pendingBytes = pending.value(1).toLongLong();
    const auto maxPendingCount = static_cast<qint64>(limits.queuedFileOffers);
    const auto maxPendingBytes = static_cast<qint64>(limits.unfinishedIncomingBytes);
    if (pendingCount >= maxPendingCount
            || offer.size > maxPendingBytes
            || pendingBytes > maxPendingBytes - offer.size) {
        recordPeerEvent(*offer.peer, EventJournal::RateLimited, EventJournal::Incoming, EventJournal::Rejected);
        offer.peer->sendFileOfferResponse(offer.fileId, FileOfferResponseStatus::Rejected);
        return;
    }

    data->state = File::FS_OFFERED;
    data->name = offer.name;
    data->direction = File::INCOMING;
    data->conversation = conversation.getId();
    data->identity = conversation.getIdentityId();
    data->contact = conversation.getFirstParticipant()->getId();
    data->identity = conversation.getIdentityId();
    data->fileId = offer.fileId;
    data->size = offer.size;
    data->hash = offer.sha512;
    data->mime_type = offer.type;
    data->thumbnail_for = offer.thumbnail_for; data->manifest_id = offer.manifest_id;
    if (offer.checkpoint_attempt) {
        data->checkpoint_interval = offer.checkpoint_attempt->interval;
        data->offered_connection = offer.connectionId;
    }

    auto file = addFile(move(data), offer.checkpoint_attempt);
    if (file) {
        if (file->isThumbnail()) {
            file->accept(); // Explicit per-identity preview consent; never accepts the primary.
            return;
        }

        const auto message = QObject::tr("Incoming file: %1 (%2 bytes) — waiting for approval")
                .arg(file->getName()).arg(file->getSize());
        DsEngine::instance().reportStatus(message, DsEngine::StatusLevel::WARNING);

        if (settings_.value("autoAcceptIncomingFiles", false).toBool()) {
            QString reason;
            if (canAcceptIncoming(*file, reason)) {
                file->accept();
                DsEngine::instance().reportStatus(
                    QObject::tr("Accepted incoming file automatically: %1").arg(file->getName()));
            } else {
                recordPeerEvent(*offer.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
                DsEngine::instance().reportStatus(
                    QObject::tr("Incoming file was not accepted: %1").arg(reason),
                    DsEngine::StatusLevel::ERROR);
            }
        }
    } else {
        DsEngine::instance().reportStatus(
            QObject::tr("Failed to receive file"),
            DsEngine::StatusLevel::ERROR);
    }
}

bool FileManager::canAcceptIncoming(const File& file, QString& reason) const
{
    if (file.getDirection() != File::INCOMING) {
        reason = QObject::tr("This is not an incoming file");
        return false;
    }

    const qint64 size = file.getSize();
    if (size < 0) {
        reason = QObject::tr("The announced file size is invalid");
        return false;
    }

    const auto limits = ResourceLimits::fromSettings(settings_);
    const qint64 maxBytes = static_cast<qint64>(limits.acceptedFileBytes);
    if (maxBytes <= 0 || size > maxBytes) {
        reason = QObject::tr("File exceeds the configured receive limit (%1 GiB)")
                .arg(static_cast<double>(limits.acceptedFileBytes)
                     / (1024.0 * 1024.0 * 1024.0));
        return false;
    }

    QSqlQuery usage;
    usage.prepare("SELECT COALESCE(SUM(size), 0) FROM file "
                  "WHERE direction=:direction AND state IN "
                  "(:queued, :transferring, :hashing)");
    usage.bindValue(":direction", static_cast<int>(File::INCOMING));
    usage.bindValue(":queued", static_cast<int>(File::FS_QUEUED));
    usage.bindValue(":transferring", static_cast<int>(File::FS_TRANSFERRING));
    usage.bindValue(":hashing", static_cast<int>(File::FS_HASHING));
    if (!usage.exec() || !usage.next()) {
        reason = QObject::tr("Unable to determine current incoming disk usage");
        return false;
    }
    const auto usedBytes = usage.value(0).toLongLong();
    const bool alreadyCounted = file.getId() != 0
            && (file.getState() == File::FS_QUEUED
                || file.getState() == File::FS_TRANSFERRING
                || file.getState() == File::FS_HASHING);
    const auto reservation = alreadyCounted ? qint64{0} : size;
    const auto diskLimit = static_cast<qint64>(limits.incomingDiskBytes);
    if (usedBytes < 0 || reservation > diskLimit
            || usedBytes > diskLimit - reservation) {
        reason = QObject::tr("The incoming transfer disk limit has been reached");
        return false;
    }

    QSqlQuery active;
    active.prepare("SELECT COUNT(*) FROM file WHERE contact_id=:contact "
                   "AND direction=:direction AND state=:state");
    active.bindValue(":contact", file.getContactId());
    active.bindValue(":direction", static_cast<int>(File::INCOMING));
    active.bindValue(":state", static_cast<int>(File::FS_TRANSFERRING));
    const auto activeCount = active.exec() && active.next()
            ? active.value(0).toLongLong() : -1;
    const auto activeOtherFiles = activeCount >= 0
            ? activeCount - (file.getState() == File::FS_TRANSFERRING ? 1 : 0) : -1;
    if (activeOtherFiles < 0 || activeOtherFiles >= limits.activeFileTransfers) {
        reason = QObject::tr("The contact has reached the active transfer limit");
        return false;
    }

    const QStorageInfo storage(QFileInfo(storage_.downloadsRoot()).absolutePath());
    if (!storage.isValid() || !storage.isReady()) {
        reason = QObject::tr("The download storage is unavailable");
        return false;
    }

    const qint64 minimumPercent = limits.minimumFreeSpacePercent;
    const qint64 available = storage.bytesAvailable();
    const qint64 total = storage.bytesTotal();
    const qint64 transferred = qMax<qint64>(0, file.getBytesTransferred());
    const qint64 remaining = qMax<qint64>(0, size - qMin(size, transferred));
    const qint64 requiredReserve = total > 0
            ? (total / 100) * minimumPercent + ((total % 100) * minimumPercent) / 100
            : 0;
    if (available < remaining || available - remaining < requiredReserve) {
        reason = QObject::tr("There is not enough free space in the download location");
        return false;
    }

    return true;
}

void FileManager::touch(const File::ptr_t &file)
{
    lru_cache_.touch(file);
}

void FileManager::onFileStateChanged(const File *file)
{
    emit fileStateChanged(file);
    if (file->isThumbnail() && file->getState() == File::FS_DONE) {
        if (auto* conversation = file->getConversation()) {
            if (auto primary = getFileFromId(file->dataThumbnailFor(), *conversation)) primary->refreshPreview();
        }
    }
}

File::ptr_t FileManager::thumbnailFor(const File& primary) {
    QSqlQuery query;
    query.prepare("SELECT t.file_id FROM file_thumbnail_v1 t JOIN file f ON f.id=t.file_id WHERE t.primary_id=? AND f.contact_id=? AND f.direction=?");
    query.addBindValue(primary.getId()); query.addBindValue(primary.getContactId()); query.addBindValue(int(primary.getDirection()));
    if (!query.exec()) throw Error("Cannot look up file thumbnail");
    if (query.next()) return getFile(query.value(0).toInt());
    return {};
}
QByteArray FileManager::thumbnailManifest(const File& primary) {
    QSqlQuery query;
    query.prepare("SELECT manifest_id FROM file_thumbnail_v1 WHERE primary_id=?");
    query.addBindValue(primary.getId());
    if (!query.exec()) throw Error("Cannot look up thumbnail manifest");
    return query.next() ? query.value(0).toByteArray() : QByteArray{};
}

void FileManager::importOutgoingFile(const File::ptr_t &file)
{
    file->importIntoStorage([file](const QByteArray& hash, const QString& failReason){
        if (hash.isEmpty()) {
            LFLOG_DEBUG << "Failed to hash file #" << file->getId()
                        << ": " << failReason;
            file->setState(File::FS_FAILED);
        } else {
            if (file->getState() == File::FS_HASHING) {
                LFLOG_DEBUG << "Calculated hash for file #" << file->getId();
                file->setHash(hash);
                DsEngine::instance().getFileManager()->prepareOutgoingOffer(file);
            } else {
                LFLOG_WARN << "Calculated hash for file #" << file->getId()
                           << " but the state was not FS_HASHING but " << file->getState();
            }
        }
    });
}



QCoro::Task<void> FileManager::prepareOutgoingOffer(File::ptr_t file) {
    auto operation = storage_.holdOperation();
    const auto still_present = [&] {
        QSqlQuery query;
        query.prepare("SELECT 1 FROM file WHERE id=? AND identity_id=? AND contact_id=?");
        query.addBindValue(file->getId()); query.addBindValue(file->getIdentityId()); query.addBindValue(file->getContactId());
        return query.exec() && query.next();
    };
    try {
        auto contact = DsEngine::instance().getContactManager()->getContact(file->getContactId());
        if (!contact || !contact->canSendFileType(file->getMimeType(), file->getName())) {
            file->setState(File::FS_REJECTED); co_return;
        }
        QPointer<Identity> identity = contact->getIdentity();
        const auto enabled = [&] {
            return identity && ((identity->getPreviewImages() && file->getMimeType().startsWith("image/"))
                || (identity->getPreviewVideo() && media_preview::isVideoCandidate(file->getMimeType())));
        };
        if (enabled()
            && contact->canSendFileType("image/jpeg", "preview.jpg")) {
            auto jpeg = co_await storage_.imagePreview(file->getId());
            if (jpeg && still_present() && file->getState() == File::FS_HASHING && enabled()) {
                auto data = std::make_unique<FileData>();
                data->state = File::FS_HASHING; data->name = "preview.jpg"; data->mime_type = "image/jpeg";
                data->identity = file->getIdentityId(); data->contact = file->getContactId(); data->conversation = file->getConversationId();
                data->size = jpeg.value().size(); data->hash = QCryptographicHash::hash(jpeg.value(), QCryptographicHash::Sha256);
                data->thumbnail_for = file->getFileId(); data->manifest_id = ds::crypto::Crypto::getRandomBytes(32);
                auto thumbnail = addFile(std::move(data));
                auto imported = co_await thumbnail->importThumbnail(jpeg.value());
                if (!imported || file->getState() != File::FS_HASHING) {
                    thumbnail->deleteFromDb();
                    co_await storage_.collectGarbage();
                }
            }
        }
    } catch (...) { LFLOG_WARN << "Optional file thumbnail preparation failed"; }
    if (still_present() && file->getState() == File::FS_HASHING) {
        file->setState(File::FS_WAITING);
        if (auto* contact = file->getContact()) contact->queueFile(file);
    }
}

}} // namespaces
