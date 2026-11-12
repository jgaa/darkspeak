#include "ds/eventjournal.h"
#include <QDateTime>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using E = ds::core::EventJournal;
class EventJournalTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;
    QSqlDatabase db_;
    std::unique_ptr<QSettings> settings_;
    qint64 scalar(const QString& sql) {
        QSqlQuery query(db_);
        if (!query.exec(sql) || !query.next()) return -1;
        return query.value(0).toLongLong();
    }
private slots:
    void init() {
        db_ = QSqlDatabase::addDatabase("QSQLITE", "events-test");
        db_.setDatabaseName(directory_.filePath("events.db"));
        QVERIFY(db_.open());
        QSqlQuery query(db_);
        QVERIFY(query.exec("DROP TABLE IF EXISTS event"));
        QVERIFY(query.exec("DROP TABLE IF EXISTS event_schema"));
        settings_ = std::make_unique<QSettings>(directory_.filePath("events.ini"), QSettings::IniFormat);
        settings_->clear();
    }
    void cleanup() {
        settings_.reset(); db_.close(); db_ = {}; QSqlDatabase::removeDatabase("events-test");
    }
    void disabled_lazy_schema_and_enable_disable() {
        E events(db_, *settings_);
        QVERIFY(!events.enabled());
        events.append(E::Message, 1, 2, E::Incoming);
        QCOMPARE(scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='event'"), 0);
        QVERIFY(events.configure(true, 60));
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), 1);
        QCOMPARE(scalar("SELECT kind FROM event"), qint64(E::RecordingEnabled));
        events.append(E::Message, 1, 2, E::Incoming, E::Success, QByteArray(32, 'x'));
        QVERIFY(events.configure(false, 60));
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), 3);
        QCOMPARE(scalar("SELECT kind FROM event ORDER BY id DESC LIMIT 1"), qint64(E::RecordingDisabled));
        events.append(E::Message);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), 3);
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY2(events.verification().contains("internally consistent"), qPrintable(events.verification()));
    }
    void invalid_stored_preferences_use_private_defaults() {
        settings_->setValue("events/enabled", "invalid");
        settings_->setValue("events/precisionSeconds", "invalid");
        { E events(db_, *settings_); QVERIFY(!events.enabled()); }
        settings_->setValue("events/enabled", true);
        E events(db_, *settings_);
        QVERIFY(events.enabled());
        QCOMPARE(scalar("SELECT precision FROM event LIMIT 1"), 60);
    }
    void unsupported_schema_is_not_silently_modified() {
        QSqlQuery query(db_);
        QVERIFY(query.exec("CREATE TABLE event_schema(version INTEGER NOT NULL)"));
        QVERIFY(query.exec("INSERT INTO event_schema VALUES(3)"));
        E events(db_, *settings_);
        QVERIFY(!events.configure(true, 60));
        QVERIFY(!events.enabled());
        QVERIFY(!events.error().isEmpty());
        QCOMPARE(scalar("SELECT version FROM event_schema"), 3);
        QCOMPARE(scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='event'"), 0);
    }
    void precision_data() {
        QTest::addColumn<int>("precision");
        for (auto value : {0, 60, 300, 1800, 3600}) QTest::newRow(qPrintable(QString::number(value))) << value;
    }
    void precision() {
        QFETCH(int, precision);
        E events(db_, *settings_);
        const auto before = QDateTime::currentSecsSinceEpoch();
        QVERIFY(events.configure(true, precision));
        const auto after = QDateTime::currentSecsSinceEpoch();
        const auto stored = scalar("SELECT timestamp FROM event LIMIT 1");
        QVERIFY(stored >= before);
        QVERIFY(stored <= after + precision);
        if (precision) QCOMPARE(stored % precision, 0);
        QVERIFY(!events.configure(true, 17));
        QCOMPARE(scalar("SELECT precision FROM event LIMIT 1"), qint64(precision));
    }
    void bounded_flood_and_summary() {
        E events(db_, *settings_);
        QVERIFY(events.configure(true, 1800));
        for (int i = 0; i < 10000; ++i) events.append(E::PolicyRejection, 1, 2, E::Incoming, E::Rejected);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), 1 + qint64(E::bucket_burst));
        QVERIFY(events.configure(false, 1800)); // Flushes one immutable summary.
        QCOMPARE(scalar("SELECT repeat_count FROM event WHERE repeat_count IS NOT NULL"), 10000 - qint64(E::bucket_burst));
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE repeat_count IS NOT NULL"), 1);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE window_start%1800<>0 OR window_end%1800<>0"), 0);
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY2(events.verification().contains("internally consistent"), qPrintable(events.verification()));
    }
    void summary_batch_rolls_back_without_losing_counts() {
        E events(db_, *settings_); QVERIFY(events.configure(true, 60));
        for (int i = 0; i < 100; ++i) {
            events.append(E::AvatarRequest, 1, 2, E::Incoming);
            events.append(E::PolicyRejection, 1, 2, E::Incoming, E::Rejected);
        }
        const auto before = scalar("SELECT COUNT(*) FROM event");
        QSqlQuery query(db_);
        QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER refuse_summary BEFORE INSERT ON event "
            "WHEN NEW.repeat_count IS NOT NULL AND NEW.kind=%1 BEGIN SELECT RAISE(ABORT,'fixture'); END").arg(int(E::PolicyRejection))));
        QVERIFY(!events.configure(true, 300));
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), before);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE repeat_count IS NOT NULL"), 0);
        QVERIFY(query.exec("DROP TRIGGER refuse_summary"));
        QVERIFY(events.configure(false, 60));
        QCOMPARE(scalar("SELECT SUM(repeat_count) FROM event"), qint64(160));
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY(events.verification().contains("internally consistent"));
    }
    void rotating_identifiers_cannot_bypass_global_limit() {
        E events(db_, *settings_);
        QVERIFY(events.configure(true, 60));
        for (int i = 1; i <= 10000; ++i) events.append(E::AvatarRequest, 1, i, E::Incoming);
        QVERIFY(scalar("SELECT COUNT(*) FROM event") <= 1 + E::global_burst + 3);
        QVERIFY(events.configure(false, 60));
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE repeat_count IS NOT NULL") <= E::max_buckets + 1, true);
        QCOMPARE(scalar("SELECT COALESCE(SUM(repeat_count),0)+(SELECT COUNT(*) FROM event WHERE kind=14 AND repeat_count IS NULL) FROM event"), qint64(10000));
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY(events.verification().contains("internally consistent"));
    }
    void remote_flood_cannot_suppress_local_identity_lifecycle() {
        E events(db_, *settings_); QVERIFY(events.configure(true, 60));
        for (int i = 1; i <= 1000; ++i) events.append(E::AvatarRequest, 1, i, E::Incoming);
        for (const auto kind : {E::IdentityCreated, E::IdentityModified, E::IdentityDeleted}) events.append(kind, 99);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE identity=99"), 3);
    }
    void clear_off_retains_only_new_genesis_and_monotonic_ids() {
        E events(db_, *settings_);
        QVERIFY(events.configure(true, 60)); events.append(E::IdentityDeleted, 42);
        QVERIFY(events.configure(false, 60));
        auto old_id = scalar("SELECT MAX(id) FROM event");
        QVERIFY(events.clear());
        QVERIFY(!events.enabled());
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), 1);
        QCOMPARE(scalar("SELECT kind FROM event"), qint64(E::Cleared));
        QVERIFY(scalar("SELECT id FROM event") > old_id);
        QCOMPARE(scalar("SELECT length(previous_hash) FROM event"), 32);
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY(events.verification().contains("internally consistent"));
    }
    void tamper_detected_data() {
        QTest::addColumn<QString>("sql");
        QTest::newRow("content") << "UPDATE event SET outcome=3 WHERE id=2";
        QTest::newRow("reference") << "UPDATE event SET contact=0 WHERE id=2";
        QTest::newRow("deletion") << "DELETE FROM event WHERE id=2";
        QTest::newRow("summary") << "UPDATE event SET repeat_count=1000 WHERE id=2";
    }
    void tamper_detected() {
        QFETCH(QString, sql);
        E events(db_, *settings_);
        QVERIFY(events.configure(true, 60)); events.append(E::IdentityCreated, 42); events.append(E::IdentityDeleted, 42);
        QSqlQuery query(db_); QVERIFY(query.exec(sql));
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY2(events.verification().contains("mismatch"), qPrintable(events.verification()));
    }
    void clear_failure_is_atomic() {
        E events(db_, *settings_);
        QVERIFY(events.configure(true, 60));
        QSqlQuery query(db_);
        QVERIFY(query.exec("CREATE TRIGGER refuse_genesis BEFORE INSERT ON event WHEN NEW.kind=3 BEGIN SELECT RAISE(ABORT,'fixture'); END"));
        QVERIFY(!events.clear()); QCOMPARE(scalar("SELECT COUNT(*) FROM event"), 1);
        QCOMPARE(scalar("SELECT kind FROM event"), qint64(E::RecordingEnabled));
        QVERIFY(query.exec("DROP TRIGGER refuse_genesis"));
    }
    void reopen_preserves_chain_and_pages_are_bounded() {
        settings_->setValue("events/enabled", true);
        {
            E events(db_, *settings_);
            for (int i = 1; i <= 110; ++i) events.append(E::Message, 1, i, E::Incoming, E::Success, QByteArray(32, 'x'));
        }
        E events(db_, *settings_);
        events.activate(true); QCOMPARE(events.rowCount(), 100); QVERIFY(events.hasOlder());
        auto newest = events.data(events.index(0), E::Row).toMap().value("id").toLongLong();
        events.older(); QVERIFY(events.rowCount() <= 100); QVERIFY(events.hasNewer());
        events.newer(); QCOMPARE(events.data(events.index(0), E::Row).toMap().value("id").toLongLong(), newest);
        events.verify(); QTRY_VERIFY(!events.verifying()); QVERIFY(events.verification().contains("internally consistent"));
    }
    void disk_budget_preserves_rows_and_allows_disabling_and_clear() {
        { E events(db_, *settings_); QVERIFY(events.configure(true, 60)); }
        QSqlQuery query(db_);
        // Synthetic bounded metadata rows exercise the hard budget without
        // spending real time waiting for the journal's rate limiter.
        QVERIFY(query.exec(QStringLiteral("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<%1) "
            "INSERT INTO event(version,timestamp,precision,kind,direction,outcome,object,previous_hash,hash) "
            "SELECT 1,1800000000,60,9,1,1,x'',zeroblob(32),zeroblob(32) FROM n").arg(E::maximum_rows - 2)));
        settings_->setValue("events/enabled", true);
        E events(db_, *settings_);
        events.append(E::Message, 1, 2, E::Incoming);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), qint64(E::maximum_rows - 1));
        QVERIFY(!events.error().isEmpty());
        QVERIFY(events.configure(false, 60));
        QVERIFY(!events.enabled());
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), qint64(E::maximum_rows));
        QVERIFY(events.clear());
        QCOMPARE(scalar("SELECT COUNT(*) FROM event"), 1);
        QVERIFY(events.configure(true, 60));
    }
    void filenames_default_on_and_opt_out_applies_to_future_file_events() {
        E events(db_, *settings_);
        QVERIFY(!events.enabled());
        events.append(E::FileOffer, 1, 2, E::Incoming, E::Pending, {}, "ignored.txt");
        QCOMPARE(scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='event'"), 0);
        QVERIFY(events.configure(true, 60));
        events.append(E::FileOffer, 1, 2, E::Incoming, E::Pending, {}, QString::fromUtf8("résumé.txt"));
        QSqlQuery query(db_);
        QVERIFY(query.exec("SELECT filename FROM event WHERE kind=11")); QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QString::fromUtf8("résumé.txt")); query.finish();
        QVERIFY(events.configure(true, 60, false));
        events.append(E::TransferState, 1, 2, E::Incoming, E::Success, {}, "private.txt");
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE filename='private.txt'"), 0);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE filename IS NOT NULL"), 1);
        QVERIFY(events.configure(true, 60, true));
        events.append(E::TransferState, 1, 2, E::Outgoing, E::Success, {}, "public.txt");
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE filename='public.txt'"), 1);
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY2(events.verification().contains("internally consistent"), qPrintable(events.verification()));
        QVERIFY(query.exec("UPDATE event SET filename='tampered.txt' WHERE filename='public.txt'"));
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY(events.verification().contains("mismatch"));
    }
    void filenames_are_bounded_and_do_not_affect_aggregation() {
        E events(db_, *settings_); QVERIFY(events.configure(true, 60));
        for (const auto& filename : QStringList{"/private/path.txt", "C:\\private.txt", "line\nbreak.txt", QString(256, 'x'), QString(128, QChar(0x00e9))})
            events.append(E::FileOffer, 1, 2, E::Incoming, E::Pending, {}, filename);
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE filename IS NOT NULL"), 0);
        for (int i = 0; i < 10000; ++i)
            events.append(E::FileOffer, 1, 2, E::Incoming, E::Pending, {}, QString::number(i) + ".txt");
        QVERIFY(events.configure(false, 60));
        QCOMPARE(scalar("SELECT SUM(repeat_count) FROM event"), qint64(10005 - E::bucket_burst));
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE repeat_count IS NOT NULL AND filename IS NOT NULL"), 0);
    }
    void stored_filename_opt_out_survives_reopening() {
        settings_->setValue("events/enabled", true);
        settings_->setValue("events/recordFilenames", false);
        E events(db_, *settings_);
        events.append(E::FileOffer, 1, 2, E::Incoming, E::Pending, {}, "private.txt");
        QCOMPARE(scalar("SELECT COUNT(*) FROM event WHERE filename IS NOT NULL"), 0);
    }
    void v1_upgrade_preserves_existing_hashes_and_sequence() {
        QSqlQuery query(db_);
        QVERIFY(query.exec("CREATE TABLE event_schema(version INTEGER NOT NULL CHECK(version=1))"));
        QVERIFY(query.exec("INSERT INTO event_schema VALUES(1)"));
        QVERIFY(query.exec("CREATE TABLE event(id INTEGER PRIMARY KEY AUTOINCREMENT,version INTEGER NOT NULL CHECK(version=1),timestamp INTEGER NOT NULL,precision INTEGER NOT NULL,kind INTEGER NOT NULL,identity INTEGER,contact INTEGER,direction INTEGER NOT NULL,outcome INTEGER NOT NULL,object BLOB NOT NULL,repeat_count INTEGER,window_start INTEGER,window_end INTEGER,previous_hash BLOB NOT NULL,hash BLOB NOT NULL)"));
        QVERIFY(query.prepare("INSERT INTO event VALUES(1,1,1800000000,60,1,NULL,NULL,0,1,x'',NULL,NULL,NULL,zeroblob(32),:hash)"));
        const auto old_hash = QByteArray::fromHex("deda8f6112ebdca49db75326d9a1e4f4d6114b562f4c8c25cd8d381427a38155");
        query.bindValue(":hash", old_hash); QVERIFY(query.exec());
        QVERIFY(query.exec("UPDATE sqlite_sequence SET seq=100 WHERE name='event'"));
        E events(db_, *settings_); events.latest();
        QCOMPARE(scalar("SELECT version FROM event_schema"), 2);
        QCOMPARE(scalar("SELECT hash_version FROM event WHERE id=1"), 1);
        QVERIFY(query.exec("SELECT hash,filename FROM event WHERE id=1")); QVERIFY(query.next());
        QCOMPARE(query.value(0).toByteArray(), old_hash); QVERIFY(query.value(1).isNull()); query.finish();
        QVERIFY(events.configure(true, 60));
        events.append(E::FileOffer, 1, 2, E::Outgoing, E::Pending, {}, "fixture.txt");
        QVERIFY(scalar("SELECT MIN(id) FROM event WHERE id>1") > 100);
        events.verify(); QTRY_VERIFY(!events.verifying());
        QVERIFY2(events.verification().contains("internally consistent"), qPrintable(events.verification()));
    }
    void chain_format_fixture() {
        const QVariantMap row{{"id", qint64(1)}, {"version", 1}, {"timestamp", qint64(1800000000)}, {"precision", 60}, {"kind", int(E::RecordingEnabled)}, {"identity", QVariant{}}, {"contact", QVariant{}}, {"direction", 0}, {"outcome", 1}, {"object", QByteArray("")}, {"repeat_count", QVariant{}}, {"window_start", QVariant{}}, {"window_end", QVariant{}}};
        QCOMPARE(E::rowHash(row, QByteArray(32, '\0')).toHex(), QByteArray("deda8f6112ebdca49db75326d9a1e4f4d6114b562f4c8c25cd8d381427a38155"));
        auto with_filename = row;
        with_filename.insert("hash_version", 2);
        with_filename.insert("filename", QStringLiteral("fixture.txt"));
        QCOMPARE(E::rowHash(with_filename, QByteArray(32, '\0')).toHex(), QByteArray("b3b33748b44697d36192bb4baf0f3bdc114f6018b74313a99313aeeb931c0379"));
    }
};
QTEST_GUILESS_MAIN(EventJournalTest)
#include "event_journal_test.moc"
