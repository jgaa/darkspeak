#include "internal.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlDriver>
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace dsfm::detail {
Catalog::Catalog(const QString& path, quint64 max_bytes, bool create) {
    registerSqliteVfs();
    connection_ = uuid(QUuid::createUuid());
    db = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db.setDatabaseName(QStringLiteral("file:/dsfm/%1/catalog.db?vfs=dsfm&mode=rw").arg(path.section('/', 4, 4)));
    db.setConnectOptions("QSQLITE_OPEN_URI;QSQLITE_BUSY_TIMEOUT=1000");
    if (!db.open()) {
        db = {}; QSqlDatabase::removeDatabase(connection_);
        fail(ErrorCode::StorageUnavailable);
    }
    try {
        auto handle = db.driver()->handle();
        require(handle.isValid() && QByteArray(handle.typeName()) == "sqlite3*", ErrorCode::UnsupportedCapability);
        auto* sqlite = *static_cast<sqlite3**>(handle.data());
        sqlite3_limit(sqlite, SQLITE_LIMIT_LENGTH, 1 << 20);
        sqlite3_limit(sqlite, SQLITE_LIMIT_SQL_LENGTH, 16384);
        sqlite3_limit(sqlite, SQLITE_LIMIT_ATTACHED, 0);
        query("PRAGMA foreign_keys=ON");
        query("PRAGMA journal_mode=DELETE");
        query("PRAGMA synchronous=FULL");
        query("PRAGMA temp_store=MEMORY");
        query("PRAGMA cache_size=-1024");
        query(QStringLiteral("PRAGMA max_page_count=%1").arg(std::max<quint64>(32, max_bytes / 4096)));
        if (create) {
            Transaction transaction(*this);
            query("CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)");
            query("CREATE TABLE entries(id TEXT PRIMARY KEY,parent TEXT REFERENCES entries(id),name TEXT NOT NULL,name_key TEXT NOT NULL,type INTEGER NOT NULL CHECK(type IN(0,1)),status INTEGER NOT NULL DEFAULT 0,size INTEGER NOT NULL DEFAULT 0 CHECK(size>=0),created INTEGER NOT NULL,modified INTEGER NOT NULL,sha BLOB,generation INTEGER NOT NULL,dev TEXT,ino TEXT,mtime TEXT,UNIQUE(parent,name_key))");
            query("CREATE INDEX entries_parent ON entries(parent,name_key,id)");
            query("CREATE TABLE pending(id TEXT PRIMARY KEY,file_id TEXT NOT NULL UNIQUE,parent TEXT NOT NULL REFERENCES entries(id),name TEXT NOT NULL,name_key TEXT NOT NULL,expected_size INTEGER,expected_sha BLOB,final_size INTEGER,state INTEGER NOT NULL DEFAULT 0,replacement INTEGER NOT NULL,reserved INTEGER NOT NULL CHECK(reserved>=0),range_generation INTEGER NOT NULL DEFAULT 0,dev TEXT NOT NULL,ino TEXT NOT NULL)");
            query("CREATE INDEX pending_names ON pending(parent,name_key)");
            query("CREATE TABLE ranges(pending TEXT NOT NULL REFERENCES pending(id) ON DELETE CASCADE,kind INTEGER NOT NULL,start INTEGER NOT NULL CHECK(start>=0),end INTEGER NOT NULL CHECK(end>start),PRIMARY KEY(pending,kind,start))");
            query("CREATE TABLE operations(sequence INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT UNIQUE NOT NULL,phase TEXT NOT NULL,payload TEXT NOT NULL)");
            setMeta("schema", "1"); setMeta("generation", "0");
            transaction.commit();
        }
        require(meta("schema") == "1", ErrorCode::Corruption);
    } catch (...) {
        db.close(); db = {}; QSqlDatabase::removeDatabase(connection_); throw;
    }
}
Catalog::~Catalog() { db.close(); db = {}; QSqlDatabase::removeDatabase(connection_); }
QSqlQuery Catalog::query(const QString& sql, const QVariantList& args) {
    QSqlQuery query(db);
    require(query.prepare(sql), ErrorCode::Corruption);
    for (auto value : args) {
        if (value.metaType().id() == QMetaType::QString && value.toString().isNull()) value = QStringLiteral("");
        query.addBindValue(value);
    }
    if (!query.exec()) {
        const auto native = query.lastError().nativeErrorCode().toInt();
        if (native == 13) fail(ErrorCode::DiskFull, native);
        if (native == 19 || native == 2067 || native == 1555) fail(ErrorCode::Conflict, native);
        fail(ErrorCode::Corruption, native);
    }
    return query;
}
QVariant Catalog::scalar(const QString& sql, const QVariantList& args) {
    auto result = query(sql, args);
    return result.next() ? result.value(0) : QVariant{};
}
void Catalog::begin() { query("BEGIN IMMEDIATE"); }
void Catalog::commit() { query("COMMIT"); }
void Catalog::rollback() { QSqlQuery query(db); query.exec("ROLLBACK"); }
QString Catalog::meta(const QString& key) { return scalar("SELECT value FROM meta WHERE key=?", {key}).toString(); }
void Catalog::setMeta(const QString& key, const QString& value) { query("INSERT INTO meta VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", {key, value}); }
quint64 Catalog::generation() { bool ok = false; auto value = meta("generation").toULongLong(&ok); require(ok && value < quint64(std::numeric_limits<qint64>::max()), ErrorCode::Corruption); return value; }
quint64 Catalog::bump() { auto next = generation() + 1; setMeta("generation", QString::number(next)); return next; }
FileInfo Catalog::entry(QUuid id) {
    require(!id.isNull(), ErrorCode::InvalidArgument);
    auto q = query("SELECT id,parent,name,type,status,size,created,modified,sha,generation FROM entries WHERE id=?", {uuid(id)});
    require(q.next(), ErrorCode::MissingEntry);
    FileInfo info;
    info.id = uuid(q.value(0));
    if (!q.value(1).isNull()) info.parent = uuid(q.value(1));
    info.name = q.value(2).toString(); info.type = EntryType(q.value(3).toInt()); info.status = EntryStatus(q.value(4).toInt());
    info.size = q.value(5).toULongLong(); info.created = QDateTime::fromMSecsSinceEpoch(q.value(6).toLongLong(), QTimeZone::UTC);
    info.modified = QDateTime::fromMSecsSinceEpoch(q.value(7).toLongLong(), QTimeZone::UTC);
    info.sha256 = info.status == EntryStatus::Available ? q.value(8).toByteArray() : QByteArray{}; info.generation = q.value(9).toULongLong();
    return info;
}
QString Catalog::path(QUuid id, PhysicalNaming naming, quint32 depth) {
    auto info = entry(id);
    if (!info.parent) return ".";
    if (naming == PhysicalNaming::FileId) {
        require(info.type == EntryType::File, ErrorCode::InvalidArgument);
        const auto name = uuid(id);
        return "objects/" + name.left(2) + '/' + name;
    }
    QStringList components;
    for (quint32 i = 0; i <= depth; ++i) {
        if (!info.parent) return components.join('/');
        auto normalized = normalizeName(info.name);
        require(bool(normalized) && normalized.value() == info.name, ErrorCode::Corruption);
        components.prepend(info.name);
        info = entry(*info.parent);
    }
    fail(ErrorCode::Corruption);
}
PendingInfo Catalog::pending(PendingId id) {
    auto q = query("SELECT file_id,parent,name,expected_size,expected_sha,final_size,state,replacement,range_generation FROM pending WHERE id=?", {uuid(id)});
    require(q.next(), ErrorCode::MissingEntry);
    PendingInfo result;
    result.id = id; result.file_id = uuid(q.value(0)); result.parent = uuid(q.value(1)); result.name = q.value(2).toString();
    if (!q.value(3).isNull()) result.content.expected_size = q.value(3).toULongLong();
    result.content.expected_sha256 = q.value(4).toByteArray();
    if (!q.value(5).isNull()) result.final_size = q.value(5).toULongLong();
    result.state = PendingState(q.value(6).toInt()); result.replacement = q.value(7).toBool(); result.range_generation = q.value(8).toULongLong();
    return result;
}
QVector<ByteRange> Catalog::ranges(PendingId id, RangeKind kind) {
    QVector<ByteRange> result;
    auto q = query("SELECT start,end FROM ranges WHERE pending=? AND kind=? ORDER BY start", {uuid(id), int(kind)});
    while (q.next()) { require(result.size() < 65536, ErrorCode::Corruption); const auto start = q.value(0).toULongLong(); result.append({start, q.value(1).toULongLong() - start}); }
    return result;
}
void Catalog::putRanges(PendingId id, RangeKind kind, const QVector<ByteRange>& ranges) {
    query("DELETE FROM ranges WHERE pending=? AND kind=?", {uuid(id), int(kind)});
    for (auto range : ranges) query("INSERT INTO ranges VALUES(?,?,?,?)", {uuid(id), int(kind), qlonglong(range.offset), qlonglong(range.offset + range.length)});
}

#define DSFM_LIMITS(X) \
 X(max_file_size) X(max_total_bytes) X(max_staging_bytes) X(max_catalog_bytes) X(max_pending) X(max_writers) X(max_handles) X(max_entries) X(max_depth) X(max_ranges) X(max_page_size) X(max_operations) X(max_io_bytes) X(max_buffered_bytes) X(max_collision_attempts) X(max_notifications) X(max_batch)
QJsonObject limitsJson(const NamespaceLimits& limits) {
    QJsonObject json;
#define DSFM_PUT(field) json[#field] = QString::number(limits.field);
    DSFM_LIMITS(DSFM_PUT)
#undef DSFM_PUT
    return json;
}
NamespaceLimits readLimits(const QJsonObject& object) {
    NamespaceLimits limits;
#define DSFM_GET(field) { bool ok = false; const auto value = object[#field].toString().toULongLong(&ok); require(ok && value <= std::numeric_limits<decltype(limits.field)>::max(), ErrorCode::Corruption); limits.field = decltype(limits.field)(value); }
    DSFM_LIMITS(DSFM_GET)
#undef DSFM_GET
    validateLimits(limits);
    return limits;
}
void validateLimits(const NamespaceLimits& limits) {
#define DSFM_CHECK(field) require(limits.field > 0, ErrorCode::InvalidArgument);
    DSFM_LIMITS(DSFM_CHECK)
#undef DSFM_CHECK
    require(limits.max_file_size <= quint64(std::numeric_limits<qint64>::max())
        && limits.max_total_bytes <= quint64(std::numeric_limits<qint64>::max())
        && limits.max_staging_bytes <= limits.max_total_bytes && limits.max_file_size <= limits.max_total_bytes
        && limits.max_catalog_bytes >= 131072 && limits.max_catalog_bytes <= (1ULL << 30)
        && limits.max_io_bytes <= (16U << 20) && limits.max_buffered_bytes <= (256ULL << 20)
        && limits.max_operations <= 4096 && limits.max_page_size <= 1000 && limits.max_ranges <= 65536
        && limits.max_depth <= 128 && limits.max_batch <= 1000 && limits.max_pending <= 4096
        && limits.max_handles <= 4096 && limits.max_entries <= 10000000
        && limits.max_collision_attempts <= 10000 && limits.max_notifications <= 4096, ErrorCode::InvalidArgument);
}
quint64 charged(Catalog& db) {
    return db.scalar("SELECT COALESCE((SELECT sum(size) FROM entries WHERE type=0),0)+COALESCE((SELECT sum(reserved) FROM pending),0)").toULongLong();
}
void checkQuota(Namespace& ns, Catalog& db, quint64 reserve, quint64 old_reserve) {
    const auto& limits = ns.config.limits;
    require(reserve <= limits.max_file_size && reserve <= limits.max_staging_bytes, ErrorCode::QuotaExceeded);
    const auto total = charged(db);
    const auto staging = db.scalar("SELECT COALESCE(sum(reserved),0) FROM pending").toULongLong();
    require(total >= old_reserve && staging >= old_reserve, ErrorCode::Corruption);
    require(total - old_reserve <= limits.max_total_bytes - reserve && staging - old_reserve <= limits.max_staging_bytes - reserve, ErrorCode::QuotaExceeded);
}
void validateEntry(Namespace& ns, Catalog& db, const FileInfo& entry) {
    require(entry.status == EntryStatus::Available, entry.status == EntryStatus::Missing ? ErrorCode::MissingEntry : ErrorCode::ExternalModification);
    if (!entry.parent || (ns.config.naming == PhysicalNaming::FileId && entry.type == EntryType::Directory)) return;
    try {
        const auto path = db.path(entry.id, ns.config.naming, ns.config.limits.max_depth);
        auto fd = openBeneath(ns.root.value, path, O_RDONLY | O_NONBLOCK | (entry.type == EntryType::Directory ? O_DIRECTORY : 0));
        const auto found = identity(fd.value, entry.type == EntryType::Directory);
        auto q = db.query("SELECT dev,ino,mtime FROM entries WHERE id=?", {uuid(entry.id)}); q.next();
        require(found.dev == q.value(0).toULongLong() && found.ino == q.value(1).toULongLong(), ErrorCode::ExternalModification);
        if (entry.type == EntryType::File) require(found.size == entry.size && found.modified == q.value(2).toLongLong(), ErrorCode::ExternalModification);
    } catch (const Failure& failure) {
        const auto missing = failure.error.code == ErrorCode::MissingEntry;
        db.query("UPDATE entries SET status=? WHERE id=?", {int(missing ? EntryStatus::Missing : EntryStatus::Inconsistent), uuid(entry.id)});
        db.bump();
        throw;
    }
}
QByteArray cursor(quint64 generation, const QString& scope, const QString& last) {
    return QJsonDocument(QJsonObject{{"v", 1}, {"g", QString::number(generation)}, {"s", scope}, {"k", last}})
        .toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
QString readCursor(const PageRequest& page, quint64 generation, const QString& scope, quint32 max_page) {
    require(page.limit && page.limit <= max_page && page.cursor.size() <= 4096, ErrorCode::InvalidArgument);
    if (page.cursor.isEmpty()) return QStringLiteral("");
    auto bytes = QByteArray::fromBase64(page.cursor, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    require(!bytes.isEmpty() && bytes.size() <= 2048, ErrorCode::InvalidArgument);
    const auto document = QJsonDocument::fromJson(bytes);
    require(document.isObject(), ErrorCode::InvalidArgument);
    const auto object = document.object();
    require(object.size() == 4 && object["v"].toInt() == 1 && object["g"].isString()
        && object["s"].isString() && object["k"].isString(), ErrorCode::InvalidArgument);
    bool ok = false; const auto stored_generation = object["g"].toString().toULongLong(&ok);
    require(ok, ErrorCode::InvalidArgument);
    require(object["s"].toString() == scope && stored_generation == generation, ErrorCode::StaleCursor);
    return object["k"].toString();
}
} // namespace dsfm::detail
