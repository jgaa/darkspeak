
#include <QFile>
#include <QFileInfo>

#include "ds/database.h"
#include "ds/file.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

Database::Database(QSettings& settings)
    : settings_{settings}
{
    static const auto DRIVER{QStringLiteral("QSQLITE")};

    const auto dbpath = settings.value("dbpath").toString();
    const bool new_database = (dbpath == ":memory:") || (!QFileInfo(dbpath).isFile());

    if(!QSqlDatabase::isDriverAvailable(DRIVER)) {
        throw Error("Missing sqlite3 support");
    }

    LFLOG_INFO << "Opening database at \"" << dbpath << "\"";

    db_ = QSqlDatabase::addDatabase(DRIVER);
    db_.setDatabaseName(dbpath);

    if (!db_.open()) {
        LFLOG_WARN << "Failed to open database: " << dbpath;
        throw Error("Failed to open database");
    }
    if (dbpath != ":memory:") {
        QFile::setPermissions(dbpath,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    if (new_database) {
        LFLOG_NOTICE << "Creating new database at location: " << dbpath;
        createDatabase();
    }

    exec("PRAGMA foreign_keys = ON");

    QSqlQuery query("SELECT * FROM ds");
    if (!query.next()) {
        throw Error("Missing configuration record in database");
    }

    const auto dbver = query.value(DS_VERSION).toInt();
    LFLOG_DEBUG << "Database schema version is " << dbver;
    if (dbver != currentVersion) {
        LFLOG_WARN << "Database schema version is "
                   << dbver
                   << " while I expected " << currentVersion;
    }

    if (dbver < 2) {
        if (!db_.transaction()) throw Error("Cannot upgrade managed storage metadata");
        try {
            exec("ALTER TABLE file ADD COLUMN storage_namespace BLOB");
            exec("ALTER TABLE file ADD COLUMN storage_id BLOB");
            exec("ALTER TABLE file ADD COLUMN pending_id BLOB");
            exec("ALTER TABLE file ADD COLUMN mime_type TEXT");
            exec("UPDATE file SET state=7, path=NULL WHERE storage_id IS NULL");
            exec("UPDATE identity SET avatar=NULL");
            exec("UPDATE contact SET avatar=NULL");
            exec("UPDATE ds SET version=2");
            if (!db_.commit()) throw Error("Cannot commit managed storage metadata upgrade");
        } catch (...) { db_.rollback(); throw; }
    }
    exec("CREATE TABLE IF NOT EXISTS managed_namespace (id TEXT PRIMARY KEY NOT NULL, storage_root TEXT UNIQUE NOT NULL, naming INTEGER NOT NULL, area INTEGER NOT NULL, created INTEGER NOT NULL DEFAULT 0)");
    exec("CREATE TABLE IF NOT EXISTS media_asset (asset_key TEXT PRIMARY KEY NOT NULL, file_id BLOB NOT NULL, pending_id BLOB, deleted INTEGER NOT NULL DEFAULT 0, owner_identity INTEGER, owner_contact INTEGER, owner_notification INTEGER)");
    exec("CREATE TABLE IF NOT EXISTS storage_gc (storage_namespace BLOB NOT NULL, storage_id BLOB NOT NULL, pending_id BLOB, PRIMARY KEY(storage_namespace,storage_id))");
    exec("CREATE TRIGGER IF NOT EXISTS file_storage_delete AFTER DELETE ON file WHEN OLD.storage_id IS NOT NULL BEGIN INSERT OR IGNORE INTO storage_gc VALUES(OLD.storage_namespace,OLD.storage_id,OLD.pending_id); END");
    exec("CREATE TRIGGER IF NOT EXISTS file_preview_delete AFTER DELETE ON file BEGIN UPDATE media_asset SET deleted=1 WHERE asset_key>='preview/v1/' || OLD.id || '/' AND asset_key<'preview/v1/' || OLD.id || '0'; END");
    exec("CREATE TRIGGER IF NOT EXISTS contact_avatar_delete AFTER DELETE ON contact BEGIN UPDATE media_asset SET deleted=1 WHERE owner_contact=OLD.id; END");
    exec("CREATE TRIGGER IF NOT EXISTS identity_avatar_delete AFTER DELETE ON identity BEGIN UPDATE media_asset SET deleted=1 WHERE owner_identity=OLD.id; END");
    exec("CREATE TRIGGER IF NOT EXISTS notification_avatar_delete AFTER DELETE ON notification BEGIN UPDATE media_asset SET deleted=1 WHERE owner_notification=OLD.id; END");
    exec("CREATE INDEX IF NOT EXISTS ix_media_owner_identity ON media_asset(owner_identity)");
    exec("CREATE INDEX IF NOT EXISTS ix_media_owner_contact ON media_asset(owner_contact)");
    exec("CREATE INDEX IF NOT EXISTS ix_media_owner_notification ON media_asset(owner_notification)");
    exec("CREATE INDEX IF NOT EXISTS ix_file_storage ON file(storage_namespace, storage_id)");
    exec("CREATE TABLE IF NOT EXISTS message_attachment_v1 (message_id INTEGER NOT NULL REFERENCES message(id) ON DELETE CASCADE,position INTEGER NOT NULL CHECK(position>=0 AND position<16),file_id BLOB NOT NULL,name TEXT NOT NULL,size INTEGER NOT NULL,sha256 BLOB NOT NULL,mime_type TEXT NOT NULL,thumbnail_for INTEGER,PRIMARY KEY(message_id,position))");
    exec("CREATE TABLE IF NOT EXISTS file_thumbnail_v1 (file_id INTEGER PRIMARY KEY REFERENCES file(id) ON DELETE CASCADE,primary_id INTEGER NOT NULL UNIQUE REFERENCES file(id) ON DELETE CASCADE,manifest_id BLOB NOT NULL)");
    exec("CREATE INDEX IF NOT EXISTS ix_thumbnail_manifest_v1 ON file_thumbnail_v1(manifest_id)");
    exec("CREATE TRIGGER IF NOT EXISTS primary_thumbnail_delete_v1 BEFORE DELETE ON file BEGIN DELETE FROM file WHERE id IN(SELECT file_id FROM file_thumbnail_v1 WHERE primary_id=OLD.id); END");
    prepareData();

}

Database::~Database()
{
    // Close the database and remove the connection to make our tests happy (no warnings).
    const auto name = db_.connectionName();
    db_.close();
    db_ = {};
    QSqlDatabase::removeDatabase(name);
}

void Database::createDatabase()
{
    db_.transaction();

    try {
        exec(R"(CREATE TABLE "ds" ( `version` INTEGER NOT NULL))");
        exec(R"(CREATE TABLE "identity" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `uuid` BLOB NOT NULL UNIQUE, `hash` BLOB NOT NULL, `name` TEXT NOT NULL UNIQUE, `cert` BLOB NOT NULL, `address` TEXT, `address_data` TEXT, `notes` TEXT, `avatar` BLOB, `created` TEXT NOT NULL, `auto_connect` INTEGER NOT NULL DEFAULT 1 ))");
        exec(R"(CREATE TABLE "contact" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `identity` INTEGER NOT NULL, `uuid` BLOB NOT NULL UNIQUE, `name` TEXT, `nickname` TEXT, `cert` BLOB NOT NULL, `address` TEXT NOT NULL, `notes` TEXT, `contact_group` TEXT NOT NULL DEFAULT 'other', `avatar` BLOB, `created` TEXT NOT NULL, `initiated_by` TEXT NOT NULL, `last_seen` TEXT, `state` INTEGER NOT NULL DEFAULT 0, `addme_message` TEXT DEFAULT 0, `auto_connect` INTEGER NOT NULL DEFAULT 0, `hash` BLOB NOT NULL, `peer_verified` INTEGER DEFAULT 0, `manually_disconnected` INTEGER DEFAULT 0, `download_path` TEXT, `sent_avatar` INTEGER DEFAULT 0, blocked int, notify_blocked int, bio TEXT, bio_hash BLOB, FOREIGN KEY(`identity`) REFERENCES `identity`(`id`) ))");
        exec(R"(CREATE TABLE "conversation" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `identity` INTEGER NOT NULL, `type` INTEGER NOT NULL DEFAULT 0, `name` TEXT NOT NULL, `uuid` INTEGER, `hash` BLOB NOT NULL, `participants` TEXT, `topic` TEXT, `created` TEXT NOT NULL, `updated` TEXT NOT NULL, `unread` INTEGER, FOREIGN KEY(`identity`) REFERENCES `identity`(`id`) ))");
        exec(R"(CREATE TABLE "message" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `direction` INTEGER NOT NULL, `state` INTEGER NOT NULL, `conversation_id` INTEGER NOT NULL, `conversation` BLOB NOT NULL, `message_id` BLOB NOT NULL, `composed_time` INTEGER NOT NULL, `received_time` INTEGER, `content` TEXT NOT NULL, `signature` BLOB NOT NULL, `sender` BLOB NOT NULL, `encoding` INTEGER NOT NULL, FOREIGN KEY(`conversation_id`) REFERENCES `conversation`(`id`) ON DELETE CASCADE ))");
        exec(R"(CREATE TABLE "notification" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `status` INTEGER NOT NULL, `priority` INTEGER NOT NULL, `identity` INTEGER NOT NULL, `contact` INTEGER, `type` INTEGER NOT NULL, `timestamp` TEXT NOT NULL, `message` TEXT, `data` BLOB, `hash` BLOB, FOREIGN KEY(`contact`) REFERENCES `contact`(`id`) ON DELETE CASCADE ))");
        exec(R"(CREATE TABLE "file" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `file_id` BLOB NOT NULL, `state` INTEGER, `direction` INTEGER, `identity_id` INTEGER NOT NULL, `conversation_id` INTEGER, `contact_id` INTEGER NOT NULL, `hash` BLOB, `name` TEXT NOT NULL, `path` TEXT, `size` INTEGER NOT NULL, `file_time` TEXT, `created_time` TEXT NOT NULL, `ack_time` TEXT, `bytes_transferred` INTEGER DEFAULT 0, FOREIGN KEY(`conversation_id`) REFERENCES `conversation`(`id`) ON DELETE CASCADE, FOREIGN KEY(`identity_id`) REFERENCES `identity`(`id`), FOREIGN KEY(`contact_id`) REFERENCES `contact`(`id`) ON DELETE CASCADE ))");
        exec(R"(CREATE UNIQUE INDEX `ix_contact_hash` ON `contact` ( `identity`, `hash` ))");
        exec(R"(CREATE UNIQUE INDEX `ix_contact_name` ON `contact` ( `identity`, `name` ))");
        exec(R"(CREATE UNIQUE INDEX `ix_message_id` ON `message` (`conversation_id` ,`id` ))");
        exec(R"(CREATE UNIQUE INDEX `ix_message_global_id` ON `message` (`message_id` ))");
        QSqlQuery query(db_);
        query.prepare("INSERT INTO ds (version) VALUES (:version)");
        query.bindValue(":version", 1);
        if(!query.exec()) {
            throw Error("Failed to initialize database");
        }

    } catch(const std::exception&) {
        db_.rollback();
        throw;
    }

    db_.commit();
}

void Database::exec(const char *sql)
{
    QSqlQuery query(db_);
    query.exec(sql);
    if (query.lastError().type() != QSqlError::NoError) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
}

void Database::prepareData()
{
    exec("CREATE INDEX IF NOT EXISTS ix_message_history ON message(conversation_id, composed_time, id)");
    exec("CREATE INDEX IF NOT EXISTS ix_file_history ON file(conversation_id, created_time, id)");
    // MessageId is globally scoped in protocol v3. Enforce that invariant in
    // persistence as well as in the application duplicate check.
    exec("CREATE UNIQUE INDEX IF NOT EXISTS ix_message_global_id ON message(message_id)");

    {
        // Live channels cannot survive restart. Preserve active/queued transfers
        // for an explicit FileResume exchange; re-offering a partial transfer
        // would reset the sender to zero. An unaccepted outgoing offer may be
        // advertised again without claiming a resumed channel.
        QSqlQuery query(db_);
        query.prepare("UPDATE file SET state=CASE WHEN direction=:out AND state=:offered "
                      "THEN :waiting ELSE :failed END WHERE state IN(:transferring,:queued) "
                      "OR (direction=:out AND state=:offered)");
        query.bindValue(":waiting", static_cast<int>(File::FS_WAITING));
        query.bindValue(":failed", static_cast<int>(File::FS_FAILED));
        query.bindValue(":out", static_cast<int>(File::OUTGOING));
        query.bindValue(":transferring", static_cast<int>(File::FS_TRANSFERRING));
        query.bindValue(":queued", static_cast<int>(File::FS_QUEUED));
        query.bindValue(":offered", static_cast<int>(File::FS_OFFERED));
        if (!query.exec()) {
            throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
        }
    }

    // Re-enable auto-connect for all contacts with that flag.
    exec("UPDATE contact SET manually_disconnected=0");
}


}} // namespaces
