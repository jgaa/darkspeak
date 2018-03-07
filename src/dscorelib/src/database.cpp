
#include <QDebug>
#include <QFileInfo>

#include "ds/database.h"

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

    QSqlQuery query("SELECT * FROM ds");
    if (!query.next()) {
        throw Error("Missing configuration record in database");
    }

    const auto dbver = query.value(DS_VERSION).toInt();
    qDebug() << "Database schema version is " << dbver;
    if (dbver != currentVersion) {
        qWarning() << "Database schema version is "
                   << dbver
                   << " while I expected " << currentVersion;
    }

}

Database::~Database()
{

}

void Database::createDatabase()
{
    db_.transaction();

    try {
        exec(R"(CREATE TABLE "ds" ( `version` INTEGER NOT NULL))");
        exec(R"(CREATE TABLE "identity" ( `id` INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `hash` BLOB NOT NULL UNIQUE, `name` TEXT NOT NULL UNIQUE, `cert` TEXT NOT NULL, `address` TEXT NOT NULL, `address_data` TEXT, `notes` TEXT, `avatar` BLOB, `created` INTEGER NOT NULL ))");
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
