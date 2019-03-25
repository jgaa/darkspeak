
#include <QDir>
#include <QSqlQuery>
#include <QStandardPaths>

#include "include/ds/filemanager.h"

#include "logfault/logfault.h"

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
    static const QRegExp forbidden{R"(\\|\/|\||\.\.)"};

    if (offer.name.count(forbidden)) {
        LFLOG_WARN << "Rejecting file #" << offer.fileId.toBase64()
                   << " from peer " << conversation.getFirstParticipant()->getName()
                   << " on identity " << conversation.getIdentity()->getName()
                   << ": Suspicious file name!";

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

    if (addFile(move(data))) {
        offer.peer->sendAck("IncomingFile", "Received", offer.fileId.toBase64());
    } else {
        offer.peer->sendAck("IncomingFile", "Failed", offer.fileId.toBase64());
    }
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
