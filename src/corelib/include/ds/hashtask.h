#ifndef HASHTASK_H
#define HASHTASK_H

#include <functional>

#include <QObject>
#include <QRunnable>

#include "ds/file.h"

namespace ds {
namespace core {

class HashTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    HashTask(QObject *owner, const File::ptr_t& file);
    void run() override;

signals:
    void hashed(const QByteArray& hash, const QString& failReason);

private:
    QString getPath() const noexcept;

    const File::ptr_t file_;
};

}}

#endif // HASHTASK_H
