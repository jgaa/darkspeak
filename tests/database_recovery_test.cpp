#include "ds/database.h"
#include "ds/file.h"
#include <QSettings>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace ds::core;
class RecoveryDatabase final : public Database {
  public:
    using Database::Database;
    using Database::prepareData;
};
class DatabaseRecoveryTest final : public QObject {
    Q_OBJECT
  private slots:
    void restart_retains_transfer_states_and_offsets() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QSettings settings(root.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("dbpath", ":memory:");
        RecoveryDatabase database(settings);
        QSqlQuery query(database.getDb());
        QVERIFY(query.exec("INSERT INTO identity(id,uuid,hash,name,cert,created) "
                           "VALUES(1,X'01',X'01','fixture',X'01','fixture')"));
        QVERIFY(query.exec("INSERT INTO contact(id,identity,uuid,cert,address,created,initiated_by,hash) "
                           "VALUES(1,1,X'02',X'02','fixture','fixture','fixture',X'02')"));
        for (int direction = 0; direction < 2; ++direction) {
            for (int state = File::FS_CREATED; state <= File::FS_CANCELLED; ++state) {
                query.prepare("INSERT INTO file(file_id,state,direction,identity_id,contact_id,name,size,"
                              "created_time,bytes_transferred) VALUES(?,?,?,?,?,?,?,?,?)");
                query.addBindValue(QByteArray(16, char(direction * 10 + state)));
                query.addBindValue(state);
                query.addBindValue(direction);
                query.addBindValue(1);
                query.addBindValue(1);
                query.addBindValue("fixture.bin");
                query.addBindValue(16384);
                query.addBindValue("fixture");
                query.addBindValue(8192);
                QVERIFY(query.exec());
            }
        }
        // Recovery must be idempotent and must preserve the durable prefix.
        database.prepareData();
        database.prepareData();
        QVERIFY(query.exec("SELECT direction,state,bytes_transferred FROM file ORDER BY id"));
        for (int direction = 0; direction < 2; ++direction) {
            for (int before = File::FS_CREATED; before <= File::FS_CANCELLED; ++before) {
                QVERIFY(query.next());
                const int expected = before == File::FS_TRANSFERRING || before == File::FS_QUEUED
                    ? File::FS_FAILED
                    : direction == File::OUTGOING && before == File::FS_OFFERED
                        ? File::FS_WAITING : before;
                QCOMPARE(query.value(0).toInt(), direction);
                QCOMPARE(query.value(1).toInt(), expected);
                QCOMPARE(query.value(2).toLongLong(), 8192);
            }
        }
        QVERIFY(!query.next());
    }
};
QTEST_GUILESS_MAIN(DatabaseRecoveryTest)
#include "database_recovery_test.moc"
