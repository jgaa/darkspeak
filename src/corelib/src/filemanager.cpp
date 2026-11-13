
#include <QFileInfo>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QCryptographicHash>

#include "include/ds/filemanager.h"
#include "ds/dsengine.h"
#include "ds/resourcelimits.h"
#include "ds/mediapreview.h"
#include "ds/crypto.h"

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
    query.prepare("SELECT id FROM file WHERE file_id=:fid AND contact_id=:cid");
    query.bindValue(":fid", fileId);
    query.bindValue(":cid", contact.getId());
    query.exec();

    if (query.next()) {
        return getFile(query.value(0).toInt());
    }

    return {};
}

File::ptr_t FileManager::addFile(std::unique_ptr<FileData> data)
{
    auto file = make_shared<File>(*this, move(data));
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

void FileManager::receivedFileOffer(Conversation& conversation, const PeerFileOffer &offer)
{
    if (!offer.thumbnail_for.isEmpty()) {
        auto primary = getFileFromId(offer.thumbnail_for, conversation);
        const auto* identity = conversation.getIdentity();
        const auto allowed = primary && !primary->isThumbnail() && primary->getDirection() == File::INCOMING
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
        if (auto file = getFileFromId(offer.fileId, conversation)) {
            if (file->getSize() != offer.size || file->getHash() != offer.sha512 || file->getName() != offer.name
                || file->getMimeType() != offer.type || file->dataThumbnailFor() != offer.thumbnail_for) {
                recordPeerEvent(*offer.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
                offer.peer->sendFileOfferResponse(offer.fileId, FileOfferResponseStatus::Rejected);
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

    auto file = addFile(move(data));
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
