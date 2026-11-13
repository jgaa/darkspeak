#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <set>
#include <deque>
#include <QUuid>
#include <QObject>
#include <QSettings>

#include "ds/conversation.h"
#include "ds/identity.h"
#include "ds/file.h"
#include "ds/registry.h"
#include "ds/lru_cache.h"
#include "ds/managedstorage.h"

namespace ds {
namespace core {

class FileManager : public QObject
{
    Q_OBJECT
public:
    explicit FileManager(QObject &parent, QSettings& settings);

    ManagedStorage& storage() { return storage_; }
    File::ptr_t getFile(const int dbId);
    File::ptr_t getFile(const QByteArray& hash, Conversation& conversation);
    File::ptr_t getFileFromId(const QByteArray& fileId, Conversation& conversation);
    File::ptr_t getFileFromId(const QByteArray& fileId, const File::Direction direction);
    File::ptr_t getFileFromId(const QByteArray& fileId, const Contact& contact);

    File::ptr_t addFile(std::unique_ptr<FileData> data);

    // Validate the local policy before an incoming file is started.
    bool canAcceptIncoming(const File& file, QString& reason) const;

    void receivedFileOffer(Conversation& conversation, const PeerFileOffer& offer);

    void touch(const File::ptr_t& file);

    void onFileStateChanged(const File *file);
    File::ptr_t thumbnailFor(const File& primary);
    QByteArray thumbnailManifest(const File& primary);

signals:
    void fileAdded(const File::ptr_t& file);
    void fileDeleted(const int dbId);
    void fileStateChanged(const core::File *file);

public slots:

private:
    void importOutgoingFile(const File::ptr_t& file);
    QCoro::Task<void> prepareOutgoingOffer(File::ptr_t file);

    Registry<int, File> registry_;
    LruCache<File::ptr_t> lru_cache_{3};
    //std::set<File::ptr_t> hashing_;
    QSettings &settings_;
    ManagedStorage storage_;
};

}}

#endif // FILEMANAGER_H
