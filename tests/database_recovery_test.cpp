#include "ds/database.h"
#include "ds/file.h"
#include <QSettings>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaEnum>
#include <array>
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
    void lifecycle_format_fixture() {
        QFile file(QFINDTESTDATA("fixtures/transfer-lifecycle-v1.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(fixture.value("version").toInt(), 1);
        const auto states = fixture.value("states").toObject();
        const auto meta = QMetaEnum::fromType<File::State>();
        QCOMPARE(states.size(), meta.keyCount());
        for (auto it = states.begin(); it != states.end(); ++it)
            QCOMPARE(meta.keyToValue(it.key().toLatin1().constData()), it.value().toInt());
        const auto reasons = fixture.value("pause_reasons").toObject();
        const std::array pairs{
            std::pair{"None", dsfm::TransferPauseReason::None},
            std::pair{"Disconnected", dsfm::TransferPauseReason::Disconnected},
            std::pair{"SourceUnavailable", dsfm::TransferPauseReason::SourceUnavailable},
            std::pair{"SourceRevalidationRequired", dsfm::TransferPauseReason::SourceRevalidationRequired},
            std::pair{"ResourceUnavailable", dsfm::TransferPauseReason::ResourceUnavailable},
            std::pair{"AuthorizationWithdrawn", dsfm::TransferPauseReason::AuthorizationWithdrawn},
            std::pair{"FeaturePaused", dsfm::TransferPauseReason::FeaturePaused}};
        QCOMPARE(reasons.size(), qsizetype(pairs.size()));
        for (const auto& [name, value] : pairs) QCOMPARE(reasons.value(name).toInt(-1), int(value));
        QTemporaryDir root;
        QSettings settings(root.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("dbpath", ":memory:");
        RecoveryDatabase database(settings);
        QSqlQuery query(database.getDb());
        QVERIFY(query.exec("PRAGMA table_info(file_transfer_v1)"));
        QJsonArray columns;
        while (query.next()) columns.append(query.value(1).toString());
        QCOMPARE(columns, fixture.value("columns").toArray());
        QCOMPARE(fixture.value("minimum_interval").toInt(), int(dsfm::minimum_checkpoint_interval));
        QCOMPARE(fixture.value("maximum_interval").toInt(), int(dsfm::maximum_checkpoint_interval));
    }
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
            for (int state = File::FS_CREATED; state <= File::FS_PAUSED_INSUFFICIENT_CREDIT; ++state) {
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
            for (int before = File::FS_CREATED; before <= File::FS_PAUSED_INSUFFICIENT_CREDIT; ++before) {
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
    void restart_preserves_checkpoint_mode_and_typed_pauses() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QSettings settings(root.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("dbpath", root.filePath("app.sqlite"));
        {
            RecoveryDatabase database(settings);
            QSqlQuery query(database.getDb());
            QVERIFY(query.exec("INSERT INTO identity(id,uuid,hash,name,cert,created) "
                               "VALUES(1,X'01',X'01','fixture',X'01','fixture')"));
            QVERIFY(query.exec("INSERT INTO contact(id,identity,uuid,cert,address,created,initiated_by,hash) "
                               "VALUES(1,1,X'02',X'02','fixture','fixture','fixture',X'02')"));
            for (const auto state : {File::FS_TRANSFERRING, File::FS_QUEUED, File::FS_OFFERED,
                                     File::FS_PAUSED, File::FS_WAITING_FOR_CREDIT, File::FS_PAUSED_INSUFFICIENT_CREDIT}) {
                query.prepare("INSERT INTO file(id,file_id,state,direction,identity_id,contact_id,name,size,created_time,bytes_transferred) "
                              "VALUES(?,?,?,0,1,1,'fixture',1048576,'fixture',?)");
                query.addBindValue(int(state)); query.addBindValue(QByteArray(16, char(state))); query.addBindValue(int(state));
                query.addBindValue(state == File::FS_OFFERED ? 0 : 65536);
                QVERIFY(query.exec());
                query.prepare("INSERT INTO file_transfer_v1(file_id,checkpoint_interval,pause_reason) VALUES(?,65536,?)");
                query.addBindValue(int(state));
                query.addBindValue(state == File::FS_PAUSED ? int(dsfm::TransferPauseReason::SourceUnavailable) : 0);
                QVERIFY(query.exec());
            }
        }
        RecoveryDatabase restarted(settings);
        restarted.prepareData(); // Idempotent even after a real connection close/reopen.
        QSqlQuery query(restarted.getDb());
        QVERIFY(query.exec("SELECT f.id,f.state,f.bytes_transferred,t.checkpoint_interval,t.pause_reason "
                           "FROM file f JOIN file_transfer_v1 t ON t.file_id=f.id ORDER BY f.id"));
        int count = 0;
        while (query.next()) {
            const auto before = File::State(query.value(0).toInt());
            const auto interrupted = before == File::FS_TRANSFERRING || before == File::FS_QUEUED;
            QCOMPARE(query.value(1).toInt(), interrupted ? int(File::FS_PAUSED) : int(before));
            QCOMPARE(query.value(2).toLongLong(), before == File::FS_OFFERED ? 0 : 65536);
            QCOMPARE(query.value(3).toUInt(), dsfm::minimum_checkpoint_interval);
            const auto reason = before == File::FS_PAUSED ? dsfm::TransferPauseReason::SourceUnavailable
                : interrupted ? dsfm::TransferPauseReason::Disconnected : dsfm::TransferPauseReason::None;
            QCOMPARE(query.value(4).toInt(), int(reason));
            ++count;
        }
        QCOMPARE(count, 6);
    }
};
QTEST_GUILESS_MAIN(DatabaseRecoveryTest)
#include "database_recovery_test.moc"
