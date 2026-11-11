#pragma once

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <vector>

namespace ds::models {
struct HistoryCursor { QString created; int type = 0; int id = 0; };
struct HistoryRecord {
    HistoryCursor cursor;
    int state = 0;
    int direction = 0;
    QDateTime received;
    QString content;
};
struct HistoryPage {
    std::vector<HistoryRecord> rows;
    bool more = false;
    bool valid = false;
};
// Call on a worker-owned, read-only SQLite connection. Each request returns at
// most 100 bounded records. The tuple cursor handles identical timestamps and
// overlapping message/file primary keys without OFFSET scans or lost rows.
inline HistoryPage readHistoryPage(QSqlDatabase &database, int conversation_id,
                                  int direction, const HistoryCursor &cursor)
{
    HistoryPage result;
    if (direction < -1 || direction > 1 || conversation_id <= 0) return result;
    const auto comparator = direction > 0 ? QStringLiteral(">") : QStringLiteral("<");
    const auto order = direction > 0 ? QStringLiteral(" ASC") : QStringLiteral(" DESC");
    QString sql = QStringLiteral(
        "SELECT * FROM ("
        "SELECT 0 AS type,id,composed_time AS created,state,direction,received_time,"
        "CASE WHEN length(CAST(content AS BLOB))<=65536 THEN content ELSE '' END AS content "
        "FROM message WHERE conversation_id=:cid "
        "UNION ALL SELECT 1 AS type,id,created_time AS created,state,direction,ack_time AS received_time,'' AS content "
        "FROM file WHERE conversation_id=:cid) ");
    if (direction != 0) sql += QStringLiteral("WHERE (created,type,id) %1 (:created,:type,:id) ").arg(comparator);
    sql += QStringLiteral("ORDER BY created%1,type%1,id%1 LIMIT 101").arg(order);
    QSqlQuery query(database);
    query.prepare(sql);
    query.bindValue(":cid", conversation_id);
    if (direction != 0) {
        query.bindValue(":created", cursor.created);
        query.bindValue(":type", cursor.type);
        query.bindValue(":id", cursor.id);
    }
    if (!query.exec()) return result;
    while (query.next()) {
        if (result.rows.size() == 100) { result.more = true; break; }
        result.rows.push_back({{query.value(2).toString(), query.value(0).toInt(), query.value(1).toInt()},
                               query.value(3).toInt(), query.value(4).toInt(), query.value(5).toDateTime(), query.value(6).toString()});
    }
    result.valid = true;
    return result;
}
}
