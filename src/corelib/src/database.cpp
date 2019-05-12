
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

    db_ = QSqlDatabase::addDatabase(DRIVER);
    db_.setDatabaseName(dbpath);

    if (!db_.open()) {
        LFLOG_WARN << "Failed to open database: " << dbpath;
        throw Error("Failed to open database");
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
        exec(R"(CREATE TABLE "contact" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `identity` INTEGER NOT NULL, `uuid` BLOB NOT NULL UNIQUE, `name` TEXT, `nickname` TEXT, `cert` BLOB NOT NULL, `address` TEXT NOT NULL, `notes` TEXT, `contact_group` TEXT NOT NULL DEFAULT 'other', `avatar` BLOB, `created` TEXT NOT NULL, `initiated_by` TEXT NOT NULL, `last_seen` TEXT, `state` INTEGER NOT NULL DEFAULT 0, `addme_message` TEXT DEFAULT 0, `auto_connect` INTEGER NOT NULL DEFAULT 0, `hash` BLOB NOT NULL, `peer_verified` INTEGER DEFAULT 0, `manually_disconnected` INTEGER DEFAULT 0, `download_path` TEXT, `sent_avatar` INTEGER DEFAULT 0, FOREIGN KEY(`identity`) REFERENCES `identity`(`id`) ))");
        exec(R"(CREATE TABLE "conversation" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `identity` INTEGER NOT NULL, `type` INTEGER NOT NULL DEFAULT 0, `name` TEXT NOT NULL, `uuid` INTEGER, `hash` BLOB NOT NULL, `participants` TEXT, `topic` TEXT, `created` TEXT NOT NULL, `updated` TEXT NOT NULL, `unread` INTEGER, FOREIGN KEY(`identity`) REFERENCES `identity`(`id`) ))");
        exec(R"(CREATE TABLE "message" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `direction` INTEGER NOT NULL, `state` INTEGER NOT NULL, `conversation_id` INTEGER NOT NULL, `conversation` BLOB NOT NULL, `message_id` BLOB NOT NULL, `composed_time` INTEGER NOT NULL, `received_time` INTEGER, `content` TEXT NOT NULL, `signature` BLOB NOT NULL, `sender` BLOB NOT NULL, `encoding` INTEGER NOT NULL ))");
        exec(R"(CREATE TABLE "notification" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `status` INTEGER NOT NULL, `priority` INTEGER NOT NULL, `identity` INTEGER NOT NULL, `contact` INTEGER, `type` INTEGER NOT NULL, `timestamp` TEXT NOT NULL, `message` TEXT, `data` BLOB, `hash` BLOB ))");
        exec(R"(CREATE TABLE "file" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `file_id` BLOB NOT NULL, `state` INTEGER, `direction` INTEGER, `identity_id` INTEGER NOT NULL, `conversation_id` INTEGER, `contact_id` INTEGER NOT NULL, `hash` BLOB, `name` TEXT NOT NULL, `path` TEXT, `size` INTEGER NOT NULL, `file_time` TEXT, `created_time` TEXT NOT NULL, `ack_time` TEXT, `bytes_transferred` INTEGER DEFAULT 0, FOREIGN KEY(`conversation_id`) REFERENCES `conversation`(`id`), FOREIGN KEY(`identity_id`) REFERENCES `identity`(`id`), FOREIGN KEY(`contact_id`) REFERENCES `contact`(`id`) ))");
        exec(R"(CREATE UNIQUE INDEX `ix_contact_hash` ON `contact` ( `identity`, `hash` ))");
        exec(R"(CREATE UNIQUE INDEX `ix_contact_name` ON `contact` ( `identity`, `name` ))");
        exec(R"(CREATE UNIQUE INDEX `ix_message_id` ON `message` (`conversation_id` ,`id` ))");
        QSqlQuery query(db_);
        query.prepare("INSERT INTO ds (version) VALUES (:version)");
        query.bindValue(":version", currentVersion);
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
    {   // Set outgoing files that was being transferred, when we last quit, to waiting state
        QSqlQuery query(db_);
        query.prepare("UPDATE file SET state=:waiting WHERE direction=:out AND state IN(:transferring, :offered, :queued)");
        query.bindValue(":waiting", static_cast<int>(File::FS_WAITING));
        query.bindValue(":out", static_cast<int>(File::OUTGOING));
        query.bindValue(":transferring", static_cast<int>(File::FS_TRANSFERRING));
        query.bindValue(":queued", static_cast<int>(File::FS_TRANSFERRING));
        query.bindValue(":offered", static_cast<int>(File::FS_OFFERED));
        query.exec();
        if (query.lastError().type() != QSqlError::NoError) {
            throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
        }
    }

    {   // Set incoming files that was being transferred, when we last quit, to queued state
        QSqlQuery query(db_);
        query.prepare("UPDATE file SET state=:queued WHERE direction=:in AND state=:transferring");
        query.bindValue(":queued", static_cast<int>(File::FS_QUEUED));
        query.bindValue(":in", static_cast<int>(File::INCOMING));
        query.bindValue(":transferring", static_cast<int>(File::FS_TRANSFERRING));
        query.exec();
        if (query.lastError().type() != QSqlError::NoError) {
            throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
        }
    }

    // Re-enable auto-connect for all contacts with that flag.
    exec("UPDATE contact SET manually_disconnected=0");
}


}} // namespaces
