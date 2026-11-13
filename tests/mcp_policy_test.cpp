#include "policy.h"
#include "store.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlRecord>
#include <QtTest>
using namespace ds::mcp;
class McpPolicyTest final : public QObject {
    Q_OBJECT
  private slots:
    void default_sensitive_reads_ask() {
        auto r = evaluate({}, "k", "i", "profile", {"c"}, {Decision::Allow}, true);
        QVERIFY(!r.denied);
        QVERIFY(r.approval);
        QVERIFY(!evaluate({}, "k", "i", "presence", {"c"}, {Decision::Allow}, false).approval);
    }
    void denial_precedence_and_scope() {
        QList<Rule> rules{{"k", "i", "send", "", Decision::Allow}, {"k", "i", "send", "c", Decision::Deny}};
        QVERIFY(evaluate(rules, "k", "i", "send", {"c"}, {Decision::Ask}, true).denied);
        rules = {{"k", "i", "send", "", Decision::Deny}, {"k", "i", "send", "c", Decision::Allow}};
        QVERIFY(evaluate(rules, "k", "i", "send", {"c"}, {Decision::Allow}, true).denied);
        QVERIFY(!evaluate(rules, "other", "i", "send", {"c"}, {Decision::Allow}, true).denied);
        QVERIFY(evaluate(rules, "other", "i", "send", {"c"}, {Decision::Allow}, true).approval);
    }
    void contact_ask_forces_fresh_and_multi_target_requires_all() {
        QList<Rule> rules{{"k", "i", "send", "", Decision::Allow}};
        QVERIFY(
            evaluate(rules, "k", "i", "send", {"a", "b"}, {Decision::Allow, Decision::Ask}, true).approval);
        QVERIFY(
            evaluate(rules, "k", "i", "send", {"a", "b"}, {Decision::Allow, Decision::Deny}, true).denied);
        rules = {{"k", "i", "send", "a", Decision::Allow}};
        QVERIFY(
            evaluate(rules, "k", "i", "send", {"a", "b"}, {Decision::Allow, Decision::Allow}, true).approval);
    }
    void versioned_schema_and_recovery() {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "mcp-policy-test");
        db.setDatabaseName(":memory:");
        QVERIFY(db.open());
        {
            Store first(db);
            first.query("INSERT INTO mcp_rules VALUES(?,?,?,?,?,?)",
                        {"rule", "key", "identity", "tool", QString{}, int(Decision::Allow)});
            auto empty = first.query("SELECT contact FROM mcp_rules");
            QVERIFY(empty.next());
            QVERIFY(!empty.value(0).isNull());
            QCOMPARE(empty.value(0).toString(), QStringLiteral(""));
            QFile fixture(QStringLiteral(MCP_FIXTURES) + "/schema-v1.json");
            QVERIFY(fixture.open(QIODevice::ReadOnly));
            const auto expected = QJsonDocument::fromJson(fixture.readAll()).object();
            auto version = first.query("SELECT version FROM mcp_schema");
            QVERIFY(version.next());
            QCOMPARE(version.value(0).toInt(), expected.value("version").toInt());
            const auto tables = expected.value("tables").toObject();
            for (auto it = tables.begin(); it != tables.end(); ++it) {
                auto table = first.query("SELECT * FROM " + it.key() + " LIMIT 0");
                for (const auto column : it->toArray())
                    QVERIFY(table.record().indexOf(column.toString()) >= 0);
            }
            for (const auto& state :
                 QStringList{"Pending", "Approved", "Executing", "Completed", "IndeterminateOperation"})
                first.query("INSERT INTO mcp_operations VALUES(?,?,?,?,?,?,?,?,?,?)",
                            {state, "k", "i", "tool", "[]", state.toUtf8(), QByteArray("fingerprint"), state,
                             1, "reference"});
            Store recovered(db);
            auto states = recovered.query("SELECT id,state,reference FROM mcp_operations");
            while (states.next()) {
                const auto id = states.value(0).toString();
                const auto expected_state = (id == "Pending" || id == "Approved") ? "Cancelled"
                                            : id == "Executing"                   ? "IndeterminateOperation"
                                                                                  : id;
                QCOMPARE(states.value(1).toString(), expected_state);
                QCOMPARE(states.value(2).toString(), "reference");
            }
            recovered.query("UPDATE mcp_schema SET version=99");
            QVERIFY_EXCEPTION_THROWN(Store unsupported(db), Error);
        }
        db.close();
        db = {};
        QSqlDatabase::removeDatabase("mcp-policy-test");
    }
};
QTEST_GUILESS_MAIN(McpPolicyTest)
#include "mcp_policy_test.moc"
