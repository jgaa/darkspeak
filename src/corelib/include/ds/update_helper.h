#ifndef UPDATE_HELPER_H
#define UPDATE_HELPER_H

#include <QObject>
#include <QSqlQuery>
#include <QSqlError>
#include <QBuffer>
#include <QImage>

#include "ds/errors.h"

namespace ds {
namespace core {


template <typename T>
void update(T *self, const char *name, const QVariant& value) {
    QByteArray sql{"UPDATE "};
    sql += self->getTableName();
    sql += " SET ";
    sql += name;
    sql += " =:value where id=:id";

    QSqlQuery query;
    query.prepare(sql);
    query.bindValue(":id", self->getId());
    query.bindValue(":value", value);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(
                        query.lastError().text()));
    }
}

// https://wiki.qt.io/How_to_Store_and_Retrieve_Image_on_SQLite
template <typename T>
void update(T *self, const char *name, const QImage& image) {
    QByteArray buffer;
    {
        QBuffer inBuffer( &buffer );
        inBuffer.open( QIODevice::WriteOnly );
        image.save(&inBuffer, "PNG");
    }

    update(self, name, buffer);
}

template <typename T, typename Obj, typename S>
bool updateIf(const char *name, const T& value, T& target, Obj *self, const S& signal) {
    if (value != target) {
        target = value;

        // Update only if the object is added to the database
        if (self->getId() > 0) {
            update(self, name, target);
        }
        emit (self->*signal)();
        return true;
    }

    return false;
}

}}

#endif // UPDATE_HELPER_H
