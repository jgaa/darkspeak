#ifndef FILEMANAGER_H
#define FILEMANAGER_H

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

signals:

public slots:
};

}}

#endif // FILEMANAGER_H
