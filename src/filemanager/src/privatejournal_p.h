#pragma once
#include "internal.h"
#include <sqlite3.h>
#include <fcntl.h>
namespace dsfm::detail {
inline void sqlCheck(int code) {
    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW)
        return;
    if ((code & 255) == SQLITE_FULL)
        fail(ErrorCode::DiskFull);
    if ((code & 255) == SQLITE_BUSY || (code & 255) == SQLITE_LOCKED)
        fail(ErrorCode::Busy);
    fail(ErrorCode::Corruption, code);
}
struct JournalStatement {
    sqlite3_stmt *statement = nullptr;
    JournalStatement(sqlite3 *db, const char *sql) { sqlCheck(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr)); }
    ~JournalStatement() { sqlite3_finalize(statement); }
    JournalStatement(const JournalStatement &) = delete;
    void text(int n, QString value) {
        auto bytes = value.toUtf8();
        sqlCheck(sqlite3_bind_text(statement, n, bytes.constData(), bytes.size(), SQLITE_TRANSIENT));
    }
    void blob(int n, QByteArray value) {
        sqlCheck(sqlite3_bind_blob(statement, n, value.constData(), value.size(), SQLITE_TRANSIENT));
    }
    void number(int n, quint64 value) {
        require(value <= quint64(std::numeric_limits<qint64>::max()), ErrorCode::QuotaExceeded);
        sqlCheck(sqlite3_bind_int64(statement, n, qint64(value)));
    }
    bool next() {
        auto code = sqlite3_step(statement);
        sqlCheck(code);
        return code == SQLITE_ROW;
    }
    QString text(int n) const {
        const auto *p = sqlite3_column_text(statement, n);
        return p ? QString::fromUtf8(reinterpret_cast<const char *>(p), sqlite3_column_bytes(statement, n)) : QString{};
    }
    QByteArray blob(int n) const {
        return QByteArray(static_cast<const char *>(sqlite3_column_blob(statement, n)),
                          sqlite3_column_bytes(statement, n));
    }
    quint64 number(int n) const {
        auto value = sqlite3_column_int64(statement, n);
        require(value >= 0, ErrorCode::Corruption);
        return quint64(value);
    }
};
struct PrivateJournal {
    sqlite3 *db = nullptr;
    ~PrivateJournal() {
        if (db)
            sqlite3_close_v2(db);
    }
    void exec(const char *sql) { sqlCheck(sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
    void open(int directory, bool create, quint64 maximum = 64ULL << 20) {
        auto file = openBeneath(directory, "catalog.db", O_RDWR | (create ? O_CREAT | O_EXCL : 0), create ? 0600 : 0);
        identity(file.value);
        registerSqliteVfs();
        auto uri = QByteArray("file:/dsfm/") + QByteArray::number(directory) + "/catalog.db?vfs=dsfm";
        sqlCheck(sqlite3_open_v2(uri.constData(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX,
                                 "dsfm"));
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        sqlite3_enable_load_extension(db, 0);
        sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 65536);
        sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
        exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-1024;");
        auto limit = QByteArray("PRAGMA max_page_count=") + QByteArray::number(maximum / 4096);
        exec(limit.constData());
    }
};
struct JournalTransaction {
    PrivateJournal &journal;
    bool done = false;
    explicit JournalTransaction(PrivateJournal &j) : journal(j) { journal.exec("BEGIN IMMEDIATE"); }
    ~JournalTransaction() {
        if (!done)
            sqlite3_exec(journal.db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        journal.exec("COMMIT");
        done = true;
    }
};
} // namespace dsfm::detail
