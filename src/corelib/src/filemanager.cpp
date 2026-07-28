
#include <regex>
#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QStandardPaths>

#include "include/ds/filemanager.h"
#include "ds/dsengine.h"

#include "logfault/logfault.h"

#include <limits>

using namespace std;

namespace ds {
namespace core {

FileManager::FileManager(QObject &parent, QSettings &settings)
    : QObject{&parent}, settings_{settings}
{
    // TODO: Load non-hashed files and start hashing them.
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
    emit fileAdded(file);

    if (file->getDirection() == File::OUTGOING
            && file->getState() == File::FS_CREATED) {
        hashIt(file);
    }

    return file;
}

void FileManager::receivedFileOffer(Conversation& conversation, const PeerFileOffer &offer)
{
    // Check if we have the file.
    try {
        if (auto file = getFileFromId(offer.fileId, conversation)) {
            if (file->getState() == File::FS_REJECTED) {
                offer.peer->sendAck("IncomingFile", "Rejected", offer.fileId.toBase64());
            } else if (file->getState() == File::FS_DONE) {
                offer.peer->sendAck("IncomingFile", "Completed", offer.fileId.toBase64());
            } else if (file->getState() == File::FS_QUEUED) {
                file->queueForTransfer();
            } else {
                offer.peer->sendAck("IncomingFile", "Received", offer.fileId.toBase64());
                file->setState(File::FS_OFFERED);
            }

            return;
        }
    } catch(const NotFoundError&) {
        ; // It's a new offer
    }

    auto data = make_unique<FileData>();

    // Validate name
    // TODO: Add Windows forbidden device names
    static const std::regex forbidden{R"(\\|\/|\||\.\.)"};
    if (std::regex_search(offer.name.toStdString(), forbidden)) {
        LFLOG_WARN << "Rejecting file #" << offer.fileId.toBase64()
                   << " from peer " << conversation.getFirstParticipant()->getName()
                   << " on identity " << conversation.getIdentity()->getName()
                   << ": Suspicious file name!";
        DsEngine::instance().reportStatus(
            QObject::tr("Rejected file with an unsafe name"),
            DsEngine::StatusLevel::WARNING);

        offer.peer->sendAck("IncomingFile", "Rejected", offer.fileId.toBase64());
        return;
    }

    data->state = File::FS_OFFERED;
    data->name = offer.name;
    data->path = conversation.getFilesLocation() + "/" + offer.name;
    data->direction = File::INCOMING;
    data->conversation = conversation.getId();
    data->identity = conversation.getIdentityId();
    data->contact = conversation.getFirstParticipant()->getId();
    data->identity = conversation.getIdentityId();
    data->fileId = offer.fileId;
    data->size = offer.size;
    data->hash = offer.sha512;

    auto file = addFile(move(data));
    if (file) {
        offer.peer->sendAck("IncomingFile", "Received", offer.fileId.toBase64());

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
                DsEngine::instance().reportStatus(
                    QObject::tr("Incoming file was not accepted: %1").arg(reason),
                    DsEngine::StatusLevel::ERROR);
            }
        }
    } else {
        DsEngine::instance().reportStatus(
            QObject::tr("Failed to receive file"),
            DsEngine::StatusLevel::ERROR);
        offer.peer->sendAck("IncomingFile", "Failed", offer.fileId.toBase64());
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

    const qint64 maxMb = settings_.value("maxIncomingFileSizeMb", 1024).toLongLong();
    const qint64 maxBytes = maxMb > 0 && maxMb <= (std::numeric_limits<qint64>::max() / (1024 * 1024))
            ? maxMb * 1024 * 1024 : 0;
    if (maxBytes <= 0 || size > maxBytes) {
        reason = QObject::tr("File exceeds the configured receive limit (%1 MB)").arg(maxMb);
        return false;
    }

    const QStorageInfo storage(QFileInfo(file.getPath()).absolutePath());
    if (!storage.isValid() || !storage.isReady()) {
        reason = QObject::tr("The download storage is unavailable");
        return false;
    }

    const qint64 configuredPercent = settings_.value(
        "minimumFreeSpacePercent", 10).toLongLong();
    const qint64 minimumPercent = qBound<qint64>(qint64{0}, configuredPercent, qint64{100});
    const qint64 available = storage.bytesAvailable();
    const qint64 total = storage.bytesTotal();
    const qint64 requiredReserve = total > 0
            ? (total / 100) * minimumPercent + ((total % 100) * minimumPercent) / 100
            : 0;
    if (available < size || available - size < requiredReserve) {
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
}

void FileManager::hashIt(const File::ptr_t &file)
{
    file->asynchCalculateHash([file](const QByteArray& hash, const QString& failReason){
        if (hash.isEmpty()) {
            LFLOG_DEBUG << "Failed to hash file #" << file->getId() << " " << file->getPath()
                        << ": " << failReason;
            file->setState(File::FS_FAILED);
        } else {
            if (file->getState() == File::FS_HASHING) {
                LFLOG_DEBUG << "Calculted hash for file #" << file->getId() << " " << file->getPath();
                file->setHash(hash);
                file->setState(File::FS_WAITING);
                if (auto contact = file->getContact()) {
                    contact->queueFile(file);
                } else {
                    LFLOG_WARN << "Failed to obtain contact for file " << file->getId() << " " << file->getPath();
                }
            } else {
                LFLOG_WARN << "Calculted hash for file #" << file->getId() << " " << file->getPath()
                           << " but the state was not FS_HASHING but " << file->getState();
            }
        }
    });
}



}} // namespaces
