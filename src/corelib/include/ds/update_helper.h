#ifndef UPDATE_HELPER_H
#define UPDATE_HELPER_H

#include <QSqlQuery>
#include <QSqlError>

namespace ds {
namespace core {

template <typename T>
void update(T *self, const char *name, const QVariant& value) {
    QByteArray sql{"UPDATE identity set "};
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

template <typename T, typename Obj, typename S>
void updateIf(const char *name, const T& value, T& target, Obj *self, const S& signal) {
    if (value != target) {
        target = value;
        update(self, name, target);
        emit (self->*signal)();
    }
}

}}

#endif // UPDATE_HELPER_H
