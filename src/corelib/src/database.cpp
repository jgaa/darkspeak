
#include <QDebug>
#include <QFileInfo>

#include "ds/database.h"

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
        qWarning() << "Failed to open database: " << dbpath;
        throw Error("Failed to open database");
    }

    if (new_database) {
        qInfo() << "Creating new database at location: " << dbpath;
        createDatabase();
    }

    QSqlQuery("PRAGMA foreign_keys = ON");

    QSqlQuery query("SELECT * FROM ds");
    if (!query.next()) {
        throw Error("Missing configuration record in database");
    }

    const auto dbver = query.value(DS_VERSION).toInt();
    LFLOG_DEBUG << "Database schema version is " << dbver;
    if (dbver != currentVersion) {
        qWarning() << "Database schema version is "
                   << dbver
                   << " while I expected " << currentVersion;
    }

}

Database::~Database()
{
    // Close the database and remove the connection to make our tests happy (no warnings).
    const auto name = db_.connectionName();
    db_.close();
    db_ = {};
    db_.removeDatabase(name);
}

void Database::createDatabase()
{
    db_.transaction();

    try {
        exec(R"(CREATE TABLE "ds" ( `version` INTEGER NOT NULL))");
        exec(R"(CREATE TABLE "identity" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `uuid` INTEGER NOT NULL, `hash` BLOB NOT NULL UNIQUE, `name` TEXT NOT NULL UNIQUE, `cert` BLOB NOT NULL, `address` TEXT NOT NULL, `address_data` TEXT, `notes` TEXT, `avatar` BLOB, `created` TEXT NOT NULL ))");
        exec(R"(CREATE TABLE "contact" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `identity` INTEGER NOT NULL, `uuid` TEXT NOT NULL UNIQUE, `hash` BLOB NOT NULL UNIQUE, `name` TEXT NOT NULL, `nickname` TEXT UNIQUE, `pubkey` TEXT NOT NULL UNIQUE, `address` TEXT NOT NULL, `notes` TEXT, `contact_group` TEXT NOT NULL DEFAULT 'other', `avatar` BLOB, `created` INTEGER NOT NULL, `initiated_by` TEXT NOT NULL, `last_seen` INTEGER, `online` INTEGER DEFAULT 0, FOREIGN KEY(`identity`) REFERENCES identity(id) ))");
        exec(R"(CREATE TABLE "conversation" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `identity` INTEGER NOT NULL, `type` INTEGER NOT NULL DEFAULT 0, `uuid` INTEGER, `hash` BLOB NOT NULL UNIQUE, `key` TEXT NOT NULL UNIQUE, `participants` TEXT, `topic` TEXT, FOREIGN KEY(`identity`) REFERENCES identity(id) ))");
        exec(R"(CREATE TABLE "message" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `direction` INTEGER NOT NULL, `conversation_id` INTEGER NOT NULL, `conversation` BLOB NOT NULL, `message_id` BLOB, `composed_time` INTEGER NOT NULL, `sent_time` INTEGER, `received_time` INTEGER, `content` TEXT, `signature` BLOB, `sender` BLOB, `encoding` INTEGER ))");
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



}} // namespaces
