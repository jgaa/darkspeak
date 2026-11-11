#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QSet>
#include "ds/historyquery.h"

class HistoryQueryTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        database_ = QSqlDatabase::addDatabase("QSQLITE", "history-fixture");
        database_.setDatabaseName(directory_.filePath("history.sqlite"));
        QVERIFY(database_.open());
        QSqlQuery query(database_);
        QVERIFY(query.exec("CREATE TABLE message(id INTEGER PRIMARY KEY,conversation_id INTEGER,composed_time TEXT,state INTEGER,direction INTEGER,received_time TEXT,content TEXT)"));
        QVERIFY(query.exec("CREATE TABLE file(id INTEGER PRIMARY KEY,conversation_id INTEGER,created_time TEXT,state INTEGER,direction INTEGER,ack_time TEXT)"));
        QVERIFY(query.exec("CREATE INDEX ix_message_history ON message(conversation_id,composed_time,id)"));
        QVERIFY(query.exec("CREATE INDEX ix_file_history ON file(conversation_id,created_time,id)"));
        QVERIFY(database_.transaction());
        for (int i = 1; i <= 750; ++i) {
            query.prepare("INSERT INTO message VALUES(:id,1,'2026-11-11T12:00:00.000',0,1,NULL,'fixture')");
            query.bindValue(":id", i);
            QVERIFY(query.exec());
            query.prepare("INSERT INTO file VALUES(:id,1,'2026-11-11T12:00:00.000',0,1,NULL)");
            query.bindValue(":id", i);
            QVERIFY(query.exec());
        }
        QVERIFY(query.exec("INSERT INTO message VALUES(751,2,'2026-11-11T12:00:00.000',0,1,NULL,'other conversation')"));
        QVERIFY(database_.commit());
    }
    void bounded_pages_and_tied_timestamps() {
        auto page = ds::models::readHistoryPage(database_, 1, 0, {});
        QVERIFY(page.valid);
        QCOMPARE(page.rows.size(), size_t(100));
        QVERIFY(page.more);
        QSet<QString> seen;
        int pages = 0;
        while (true) {
            ++pages;
            for (const auto &record : page.rows) {
                const auto key = QString::number(record.cursor.type) + ':' + QString::number(record.cursor.id);
                QVERIFY(!seen.contains(key));
                seen.insert(key);
            }
            if (!page.more) break;
            QVERIFY(pages < 20);
            page = ds::models::readHistoryPage(database_, 1, -1, page.rows.back().cursor);
            QVERIFY(page.valid);
        }
        QCOMPARE(seen.size(), 1500);
        QCOMPARE(pages, 15);
    }
    void newer_pages_restore_evicted_rows() {
        const ds::models::HistoryCursor cursor{QStringLiteral("2026-11-11T12:00:00.000"), 0, 700};
        const auto page = ds::models::readHistoryPage(database_, 1, 1, cursor);
        QVERIFY(page.valid);
        QCOMPARE(page.rows.size(), size_t(100));
        QCOMPARE(page.rows.front().cursor.type, 0);
        QCOMPARE(page.rows.front().cursor.id, 701);
        QCOMPARE(page.rows.back().cursor.type, 1);
        QCOMPARE(page.rows.back().cursor.id, 50);
    }
    void invalid_and_empty_requests() {
        QVERIFY(!ds::models::readHistoryPage(database_, -1, 0, {}).valid);
        QVERIFY(!ds::models::readHistoryPage(database_, 1, 4, {}).valid);
        const auto page = ds::models::readHistoryPage(database_, 999, 0, {});
        QVERIFY(page.valid);
        QVERIFY(page.rows.empty());
        QVERIFY(!page.more);
    }
    void cleanupTestCase() {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase("history-fixture");
    }
private:
    QTemporaryDir directory_;
    QSqlDatabase database_;
};
QTEST_GUILESS_MAIN(HistoryQueryTest)
#include "history_query_test.moc"
