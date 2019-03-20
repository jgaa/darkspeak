
#include <QSqlQuery>

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

File::ptr_t FileManager::addFile(std::unique_ptr<FileData> data)
{
    auto file = make_shared<File>(*this, move(data));
    file->addToDb();
    registry_.add(file->getId(), file);
    touch(file);
    emit fileAdded(file);

    if (file->getState() == File::FS_CREATED) {
        hashIt(file);
    }

    return file;
}

void FileManager::touch(const File::ptr_t &file)
{
    lru_cache_.touch(file);
}

void FileManager::hashIt(const File::ptr_t &file)
{
    if (hashing_.insert(file).second) {

        connect(file.get(), & File::hashCalculated, this, [this, file](const QByteArray& hash) {
            hashing_.erase(file);

            if (file->getState() == File::FS_HASHING) {
                LFLOG_DEBUG << "Calculted hash for file #" << file->getId() << " " << file->getPath();
                file->setHash(hash);
                file->setState(File::FS_WAITING);
            } else {
                LFLOG_WARN << "Calculted hash for file #" << file->getId() << " " << file->getPath()
                           << " but the state was not FS_HASHING but " << file->getState();
            }

            if (auto contact = file->getContact()) {
                contact->queueFile(file);
            } else {
                LFLOG_WARN << "Failed to obtain contact for file " << file->getId() << " " << file->getPath();
            }

            touch(file);
        });

        connect(file.get(), & File::hashCalculationFailed, this, [this, file](const QString& why) {
            hashing_.erase(file);
            LFLOG_DEBUG << "Failed to hash file #" << file->getId() << " " << file->getPath()
                        << ": " << why;
            touch(file);
        });

        file->asynchCalculateHash();
    }
}



}} // namespaces
