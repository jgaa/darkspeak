#include "store.h"
#include <QSqlError>

namespace ds::mcp {
QSqlQuery Store::query(const QString& sql, const QVariantList& values) const {
    QSqlQuery q(db_);
    if (!q.prepare(sql))
        throw Error("PersistenceFailed");
    for (const auto& value : values) {
        // Qt distinguishes a null QString from an empty string. Empty scopes
        // are real values; an invalid QVariant remains SQL NULL for COALESCE.
        if (value.metaType().id() == QMetaType::QString && value.toString().isNull())
            q.addBindValue(QStringLiteral(""));
        else
            q.addBindValue(value);
    }
    if (!q.exec())
        throw Error("PersistenceFailed");
    return q;
}
Store::Store(QSqlDatabase db) : db_(db) {
    if (!db_.transaction())
        throw Error("PersistenceFailed");
    try {
        query("CREATE TABLE IF NOT EXISTS mcp_schema (version INTEGER NOT NULL)");
        auto version = query("SELECT version FROM mcp_schema");
        if (version.next()) {
            if (version.value(0).toInt() != 1)
                throw Error("UnsupportedMcpSchema");
        } else {
            query("INSERT INTO mcp_schema VALUES(1)");
            query("CREATE TABLE mcp_retry_epoch (singleton INTEGER PRIMARY KEY CHECK(singleton=1), token "
                  "TEXT NOT NULL, created INTEGER NOT NULL)");
            query("CREATE TABLE mcp_keys (id TEXT PRIMARY KEY, name TEXT NOT NULL, verifier BLOB NOT NULL, "
                  "identities BLOB NOT NULL, capabilities BLOB NOT NULL, created INTEGER NOT NULL, expires "
                  "INTEGER NOT NULL, last_used INTEGER NOT NULL DEFAULT 0, enabled INTEGER NOT NULL DEFAULT "
                  "1)");
            query("CREATE TABLE mcp_rules (id TEXT PRIMARY KEY, key_id TEXT NOT NULL, identity TEXT NOT "
                  "NULL, tool TEXT NOT NULL, contact TEXT NOT NULL, decision INTEGER NOT NULL, "
                  "UNIQUE(key_id,identity,tool,contact))");
            query("CREATE TABLE mcp_contacts (identity TEXT NOT NULL, contact TEXT NOT NULL, permission TEXT "
                  "NOT NULL, decision INTEGER NOT NULL, PRIMARY KEY(identity,contact,permission))");
            query("CREATE TABLE mcp_operations (id TEXT PRIMARY KEY, key_id TEXT NOT NULL, identity TEXT NOT "
                  "NULL, tool TEXT NOT NULL, targets BLOB NOT NULL, retry_hash BLOB NOT NULL, fingerprint "
                  "BLOB NOT NULL, state TEXT NOT NULL, created INTEGER NOT NULL, reference TEXT NOT NULL "
                  "DEFAULT '', UNIQUE(key_id,identity,tool,retry_hash))");
            query("CREATE INDEX mcp_operations_created ON mcp_operations(created)");
        }
        // Pending arguments are deliberately memory-only. Never revive an old
        // approval or replay an effect that may have committed before a crash.
        query("UPDATE mcp_operations SET state='Cancelled' WHERE state IN ('Pending','Approved')");
        query("UPDATE mcp_operations SET state='IndeterminateOperation' WHERE state='Executing'");
        if (!db_.commit())
            throw Error("PersistenceFailed");
    } catch (...) {
        db_.rollback();
        throw;
    }
}
} // namespace ds::mcp
