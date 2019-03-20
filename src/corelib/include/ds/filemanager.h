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

namespace ds {
namespace core {

class FileManager : public QObject
{
    Q_OBJECT
public:
    explicit FileManager(QObject &parent, QSettings& settings);

    File::ptr_t getFile(const int dbId);
    File::ptr_t getFile(const QByteArray& hash, Conversation& conversation);

    File::ptr_t addFile(std::unique_ptr<FileData> data);

    void touch(const File::ptr_t& file);

signals:
    void fileAdded(const File::ptr_t& file);
    void fileDeleted(const int dbId);

public slots:

private:
    void hashIt(const File::ptr_t& file);

    Registry<int, File> registry_;
    LruCache<File::ptr_t> lru_cache_{3};
    std::set<File::ptr_t> hashing_;
    QSettings &settings_;
};

}}

#endif // FILEMANAGER_H
