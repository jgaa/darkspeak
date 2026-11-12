#include "internal.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <logfault/logfault.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

namespace dsfm::detail {
static std::optional<Identity> inspect(Namespace& ns, const QString& path, bool directory, bool strict = true) {
    try {
        if (!strict) {
            auto parent = openBeneath(ns.root.value, parentPath(path), O_RDONLY | O_DIRECTORY);
            struct stat st{};
            ioCheck(::fstatat(parent.value, path.section('/',-1).toUtf8().constData(), &st, AT_SYMLINK_NOFOLLOW) == 0);
            return Identity{quint64(st.st_dev), quint64(st.st_ino), quint64(st.st_size), qint64(st.st_mtim.tv_sec)*1000000000LL+st.st_mtim.tv_nsec, S_ISDIR(st.st_mode)};
        }
        auto fd = openBeneath(ns.root.value, path, O_RDONLY | O_NONBLOCK | (directory ? O_DIRECTORY : 0));
        return identity(fd.value, directory);
    } catch (const Failure& error) {
        if (error.error.code == ErrorCode::MissingEntry) return {};
        throw;
    }
}
static bool matches(const std::optional<Identity>& found, const QJsonObject& op, const QString& prefix = {}) {
    return found && found->dev == op[prefix + "dev"].toString().toULongLong()
        && found->ino == op[prefix + "ino"].toString().toULongLong();
}
QByteArray verifyContent(Namespace& ns, Catalog&, const PendingInfo& pending, const QString& path, CancellationToken cancel) {
    require(pending.final_size.has_value(), ErrorCode::IncompleteContent);
    auto fd = openBeneath(ns.root.value, path, O_RDONLY | O_NONBLOCK);
    const auto before = identity(fd.value);
    require(before.size == *pending.final_size, ErrorCode::IncompleteContent);
    if (pending.content.expected_size) require(before.size == *pending.content.expected_size, ErrorCode::IntegrityError);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray bytes(qsizetype(ns.config.limits.max_io_bytes), Qt::Uninitialized);
    quint64 offset = 0;
    while (offset < before.size) {
        cancelCheck(cancel);
        const auto count = std::min<quint64>(quint64(bytes.size()), before.size - offset);
        const auto got = ::pread(fd.value, bytes.data(), size_t(count), off_t(offset));
        if (got < 0 && errno == EINTR) continue;
        ioCheck(got >= 0); require(got > 0, ErrorCode::ExternalModification);
        hash.addData(QByteArrayView(bytes.constData(), got)); offset += quint64(got);
    }
    cancelCheck(cancel);
    const auto after = identity(fd.value);
    require(before.size == after.size && before.modified == after.modified, ErrorCode::ExternalModification);
    if (!pending.content.expected_sha256.isEmpty()) require(hash.result() == pending.content.expected_sha256, ErrorCode::IntegrityError);
    return hash.result();
}
static void applyCatalog(Namespace& ns, Catalog& db, const QJsonObject& op) {
    const auto kind = op["kind"].toString();
    const auto id = op["file_id"].toString();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto generation = db.bump();
    const auto parent = op["parent"].toString();
    const auto name = op["name"].toString();
    if (kind == "create" || kind == "mkdir") {
        db.query("INSERT INTO entries(id,parent,name,name_key,type,size,created,modified,sha,generation,dev,ino,mtime) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {id, parent, name, nameKey(name).value(), kind == "mkdir" ? 1 : 0, op["size"].toString().toLongLong(), now, now,
             QByteArray::fromHex(op["sha"].toString().toLatin1()), qlonglong(generation), op["dev"].toString(), op["ino"].toString(), op["mtime"].toString()});
    } else if (kind == "replace") {
        db.query("UPDATE entries SET size=?,modified=?,sha=?,generation=?,dev=?,ino=?,mtime=?,status=0 WHERE id=?",
            {op["size"].toString().toLongLong(), now, QByteArray::fromHex(op["sha"].toString().toLatin1()), qlonglong(generation), op["dev"].toString(), op["ino"].toString(), op["mtime"].toString(), id});
    } else if (kind == "rename" || kind == "move") {
        db.query("UPDATE entries SET parent=?,name=?,name_key=?,modified=?,generation=? WHERE id=?", {parent, name, nameKey(name).value(), now, qlonglong(generation), id});
    } else if (kind == "abort") {}
    else if (kind == "delete") db.query("DELETE FROM entries WHERE id=?", {id});
    else fail(ErrorCode::Corruption);
    if (op.contains("pending_id")) db.query("DELETE FROM pending WHERE id=?", {op["pending_id"].toString()});
    Q_UNUSED(ns);
}
static void validateOperationPath(const QString& path) {
    require(!path.isEmpty() && path.size() <= 32768 && !path.startsWith('/'), ErrorCode::Corruption);
    const auto parts = path.split('/');
    if (parts[0] == ".filemanager") {
        require(parts.size() == 3 && (parts[1] == "staging" || parts[1] == "recovery"), ErrorCode::Corruption);
        const auto id = QUuid(parts[2]); require(!id.isNull() && uuid(id) == parts[2], ErrorCode::Corruption);
    } else if (parts[0] == "objects") {
        require(parts.size() == 3 && parts[1].size() == 2, ErrorCode::Corruption);
        const auto id = QUuid(parts[2]); require(!id.isNull() && uuid(id) == parts[2] && parts[1] == parts[2].left(2), ErrorCode::Corruption);
    } else {
        require(parts.size() <= 128, ErrorCode::Corruption);
        for (const auto& part : parts) require(bool(normalizeName(part)), ErrorCode::Corruption);
    }
}
static MutationResult finishOperation(Namespace& ns, Catalog& db, QJsonObject op, QString phase, bool recovering) {
    const auto operation_id = op["id"].toString();
    const auto kind = op["kind"].toString();
    const auto from = op["from"].toString();
    const auto to = op["to"].toString();
    const bool directory = op["directory"].toBool();
    const bool catalog_only = op["catalog_only"].toBool();
    require(!QUuid(operation_id).isNull() && !QUuid(op["file_id"].toString()).isNull(), ErrorCode::Corruption);
    require(kind == "create" || kind == "replace" || kind == "mkdir" || kind == "rename" || kind == "move" || kind == "delete" || kind == "abort", ErrorCode::Corruption);
    if (!catalog_only) { validateOperationPath(from); validateOperationPath(to); }
    else require(kind != "create" && kind != "replace", ErrorCode::Corruption);
    bool sync_failed = op["sync_failed"].toBool();
    if (phase == "DurabilityPending") {
        try {
            fault("publication_sync");
            syncDirectory(ns.root.value, parentPath(from)); syncDirectory(ns.root.value, parentPath(to));
            sync_failed = false; phase = "CleanupPending";
        } catch (const Failure&) { sync_failed = true; }
    }
    if (phase == "Prepared") {
        if (!catalog_only) {
            auto source = inspect(ns, from, directory);
            auto destination = inspect(ns, to, directory, false);
            if (!matches(destination, op)) {
                require(matches(source, op), ErrorCode::ExternalModification);
                if (kind == "replace") require(matches(destination, op, "old_"), ErrorCode::ExternalModification);
                else if (destination) {
                    db.query("UPDATE operations SET phase='RolledBack' WHERE id=?", {operation_id});
                    fail(ErrorCode::Conflict);
                }
                if (recovering && (kind == "create" || kind == "replace")) {
                    PendingInfo pending;
                    pending.final_size = op["size"].toString().toULongLong();
                    pending.content.expected_sha256 = QByteArray::fromHex(op["sha"].toString().toLatin1());
                    verifyContent(ns, db, pending, from, {});
                }
                fault("before_filesystem");
                if (kind == "replace") {
#ifdef __linux__
                    // Exchange keeps the old committed generation in staging until catalog commit.
                    auto source_dir = openBeneath(ns.root.value, parentPath(from), O_RDONLY | O_DIRECTORY);
                    auto target_dir = openBeneath(ns.root.value, parentPath(to), O_RDONLY | O_DIRECTORY);
                    ioCheck(::syscall(SYS_renameat2, source_dir.value, from.section('/', -1).toUtf8().constData(),
                                     target_dir.value, to.section('/', -1).toUtf8().constData(), RENAME_EXCHANGE) == 0);
#else
                    fail(ErrorCode::UnsupportedCapability);
#endif
                } else {
                    try { renameSafe(ns.root.value, from, to); }
                    catch (const Failure& failure) {
                        if (failure.error.code == ErrorCode::Conflict) db.query("UPDATE operations SET phase='RolledBack' WHERE id=?", {operation_id});
                        throw;
                    }
                }
                fault("after_filesystem");
            }
            try { fault("publication_sync"); syncDirectory(ns.root.value, parentPath(from)); syncDirectory(ns.root.value, parentPath(to)); }
            catch (const Failure&) { sync_failed = true; }
        }
        op["sync_failed"] = sync_failed;
        db.query("UPDATE operations SET phase='FilesystemApplied',payload=? WHERE id=?", {QString::fromUtf8(QJsonDocument(op).toJson(QJsonDocument::Compact)), operation_id});
        phase = "FilesystemApplied";
        fault("after_filesystem_record");
    }
    if (recovering && (kind == "create" || kind == "replace") && phase != "Complete") {
        PendingInfo pending;
        pending.final_size = op["size"].toString().toULongLong();
        pending.content.expected_sha256 = QByteArray::fromHex(op["sha"].toString().toLatin1());
        require(matches(inspect(ns, to, false), op), ErrorCode::ExternalModification);
        verifyContent(ns, db, pending, to, {});
    }
    if (phase == "FilesystemApplied") {
        Transaction transaction(db);
        applyCatalog(ns, db, op);
        op["generation"] = QString::number(db.generation());
        db.query("UPDATE operations SET phase='CatalogApplied',payload=? WHERE id=?", {QString::fromUtf8(QJsonDocument(op).toJson(QJsonDocument::Compact)), operation_id});
        transaction.commit();
        phase = "CatalogApplied";
        LFLOG_TRACE << "FileManager logical publication committed; kind=" << kind.toStdString();
        fault("after_catalog");
    }
    if (phase == "CatalogApplied" || phase == "CleanupPending") {
        db.query("UPDATE operations SET phase='CleanupPending' WHERE id=?", {operation_id});
        if (!catalog_only && (kind == "delete" || kind == "abort" || kind == "replace")) {
            const auto cleanup = kind == "replace" ? from : to;
            auto found = inspect(ns, cleanup, directory);
            if (found) {
                require(matches(found, op, kind == "replace" ? "old_" : ""), ErrorCode::ExternalModification);
                unlinkSafe(ns.root.value, cleanup, directory);
            }
        }
        fault("after_cleanup");
        op["sync_failed"] = sync_failed;
        db.query("UPDATE operations SET phase=?,payload=? WHERE id=?", {sync_failed ? "DurabilityPending" : "Complete", QString::fromUtf8(QJsonDocument(op).toJson(QJsonDocument::Compact)), operation_id});
    }
    // Fixed status-history bound; active records are never evicted.
    db.query("DELETE FROM operations WHERE phase IN('Complete','RolledBack') AND sequence < (SELECT COALESCE(max(sequence),0)-1024 FROM operations)");
    return {QUuid(operation_id), op["generation"].toString().toULongLong(), sync_failed ? Durability::SyncFailed : Durability::Synchronized};
}
MutationResult applyOperation(Namespace& ns, Catalog& db, QJsonObject op) {
    const auto id = QUuid::createUuid(); op["id"] = uuid(id); op["version"] = 1;
    require(db.scalar("SELECT count(*) FROM operations WHERE phase NOT IN('Complete','RolledBack')").toUInt() < ns.config.limits.max_operations, ErrorCode::QueueFull);
    db.query("INSERT INTO operations(id,phase,payload) VALUES(?,'Prepared',?)", {uuid(id), QString::fromUtf8(QJsonDocument(op).toJson(QJsonDocument::Compact))});
    LFLOG_TRACE << "FileManager durable operation prepared; kind=" << op["kind"].toString().toStdString();
    fault("after_prepare");
    try { return finishOperation(ns, db, op, "Prepared", false); }
    catch (const Failure& failure) {
        if (db.scalar("SELECT phase FROM operations WHERE id=?", {uuid(id)}).toString() == "RolledBack") throw;
        Error error{ErrorCode::IndeterminateOperation, failure.error.native_code}; error.operation_id = id;
        throw Failure{error};
    }
}
void recover(Namespace& ns, Catalog& db) {
    // Read at most the admitted outstanding-operation limit; no unbounded recovery snapshot.
    auto q = db.query("SELECT id,phase,payload FROM operations WHERE phase NOT IN('Complete','RolledBack') ORDER BY sequence LIMIT ?", {4097});
    struct Item { QString id, phase; QJsonObject op; };
    QVector<Item> items;
    while (q.next()) {
        require(items.size() < 4096, ErrorCode::Corruption);
        const auto payload = q.value(2).toByteArray(); require(payload.size() <= 65536, ErrorCode::Corruption);
        auto document = QJsonDocument::fromJson(payload); require(document.isObject(), ErrorCode::Corruption);
        const auto phase = q.value(1).toString();
        require(phase == "Prepared" || phase == "FilesystemApplied" || phase == "CatalogApplied" || phase == "CleanupPending" || phase == "DurabilityPending", ErrorCode::Corruption);
        auto op = document.object(); require(op["version"].toInt() == 1 && op["id"].toString() == q.value(0).toString(), ErrorCode::Corruption);
        items.append({q.value(0).toString(), q.value(1).toString(), op});
    }
    q.finish();
    for (auto& item : items) {
        LFLOG_TRACE << "FileManager recovering durable operation; phase=" << item.phase.toStdString();
        try { finishOperation(ns, db, item.op, item.phase, true); }
        catch (const Failure& failure) {
            if (db.scalar("SELECT phase FROM operations WHERE id=?", {item.id}).toString() == "RolledBack") continue;
            Error error{ErrorCode::IndeterminateOperation, failure.error.native_code}; error.operation_id = QUuid(item.id); throw Failure{error};
        }
    }
}
} // namespace dsfm::detail
