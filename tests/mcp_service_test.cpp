#include "ds/crypto.h"
#include "ds/dsengine.h"
#include "ds/notificationsmodel.h"
#include "service.h"
#ifdef DARKSPEAK_WITH_TEST_MCP
#include "test_support.h"
#include "ds/base32.h"
#endif
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
using namespace ds::mcp;
using namespace ds::core;
class McpServiceTest final : public QObject {
    Q_OBJECT
    QTemporaryDir root_;
    std::unique_ptr<ds::crypto::Crypto> crypto_;
    std::unique_ptr<DsEngine> engine_;
    std::unique_ptr<Service> service_;
    QString identity_, contact_, key_, secret_;
    int port_ = 0;
    std::shared_ptr<Reply> call(const QString& tool, QJsonObject args) {
        auto reply = std::make_shared<Reply>();
        if (service_->reserveDispatch())
            service_->submit(key_, tool, std::move(args), reply);
        return reply;
    }
    QString retry(const QString& suffix) {
        auto response = call("status", {{"identity", identity_}});
        return response->value.value("retry_epoch").toString() + ":" + suffix;
    }
    QJsonObject profile(const QString& suffix) {
        return {{"identity", identity_}, {"contact", contact_}, {"idempotency_key", retry(suffix)}};
    }
    QJsonObject send(const QString& suffix) {
        return {{"identity", identity_},
                {"contacts", QJsonArray{contact_}},
                {"text", "fixture message"},
                {"idempotency_key", retry(suffix)}};
    }
    int messages() {
        QSqlQuery q("SELECT count(*) FROM message");
        return q.next() ? q.value(0).toInt() : -1;
    }
    QString pendingId() {
        return service_->pending().first().toMap().value("id").toString();
    }
  private slots:
    void initTestCase() {
        QVERIFY(root_.isValid());
        qputenv("DARKSPEAK_PROFILE_ROOT", root_.path().toUtf8());
        qputenv("XDG_DATA_HOME", root_.filePath("xdg-data").toUtf8());
        qputenv("XDG_CONFIG_HOME", root_.filePath("xdg-config").toUtf8());
        qputenv("XDG_CACHE_HOME", root_.filePath("xdg-cache").toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", "mcp-disposable-test-vault");
        crypto_ = std::make_unique<ds::crypto::Crypto>();
    }
    void init() {
        service_.reset();
        engine_.reset();
        const auto path = root_.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(QDir().mkpath(path));
        auto settings = std::make_unique<QSettings>(path + "/settings.ini", QSettings::IniFormat);
        settings->setValue("dbpath", path + "/app.sqlite");
        settings->setValue("downloadLocation", path + "/downloads");
        engine_ = std::make_unique<DsEngine>(std::move(settings));
        IdentityData data;
        data.uuid = QUuid::createUuid();
        data.name = "MCP fixture";
        data.cert = ds::crypto::DsCert::create();
        data.hash = data.cert->getHash().toByteArray();
        data.autoConnect = false;
        auto* owner = engine_->getIdentityManager()->addIdentity(data);
        QVERIFY(owner);
        identity_ = owner->getUuid().toString(QUuid::WithoutBraces);
        auto contact = std::make_unique<ContactData>();
        contact->identity = owner->getId();
        contact->name = "Fixture peer";
        contact->cert = ds::crypto::DsCert::create();
        contact->address = "fixture-not-networked";
        contact->autoConnect = false;
        contact->state = Contact::ACCEPTED;
        contact->bio = "private fixture biography";
        contact_ = engine_->getContactManager()
                       ->addContact(std::move(contact))
                       ->getUuid()
                       .toString(QUuid::WithoutBraces);
        service_ = std::make_unique<Service>(*engine_);
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        port_ = probe.serverPort();
        probe.close();
        QVERIFY(service_->configure(true, "127.0.0.1", port_, 1));
        const auto created = service_->createKey(
            "Fixture key", {identity_},
            {"read_status", "read_contacts", "read_profiles", "send_messages", "read_events"});
        QVERIFY2(!created.isEmpty(), qPrintable(service_->error()));
        key_ = created.value("id").toString();
        secret_ = created.value("secret").toString();
    }
    void cleanup() {
        service_.reset();
        engine_.reset();
    }
    void credentials_and_missing_ui() {
        QCOMPARE(service_->authenticate("Bearer " + secret_.toStdString()), key_);
        QVERIFY(service_->authenticate("Bearer " + std::string(64, '0')).isEmpty());
        auto response = call("profile", profile("missing-ui"));
        QVERIFY(response->ready);
        QCOMPARE(response->error, "ApprovalUnavailable");
        QVERIFY(service_->setRule(key_, identity_, "profile", contact_, "Allow"));
        response = call("profile", profile("persistent"));
        QTRY_VERIFY(response->ready);
        QCOMPARE(response->value.value("bio").toString(), "private fixture biography");
        QVERIFY(service_->revokeKey(key_));
        QVERIFY(service_->authenticate("Bearer " + secret_.toStdString()).isEmpty());
    }
#ifdef DARKSPEAK_WITH_TEST_MCP
    void isolated_probe_cannot_bypass_profile_denials_or_change_preview_target() {
        QVERIFY(QDir().mkpath(engine_->settings().value("downloadLocation").toString()));
        TestSupport support(*engine_, *service_, root_.path(), QByteArray(64, 'a'));
        QTRY_VERIFY_WITH_TIMEOUT(engine_->property("mcpTestStorageReady").toBool(), 5000);
        const QByteArray public_key(32, 'p');
        const auto checksum = QCryptographicHash::hash(
            QByteArray(".onion checksum", 15) + public_key + QByteArray(1, 3), QCryptographicHash::Sha3_256);
        const auto address = ds::crypto::onionv3encode(public_key + checksum.left(2) + QByteArray(1, 3)) + ".onion:12345";
        auto contact = engine_->getContactManager()->getContact(QUuid(contact_));
        contact->setAddress(address);
        const auto sharing = QString::fromLatin1(DsEngine::getIdentityAsBase58(contact->getCert(), address));
        QVERIFY(!sharing.isEmpty());
        QVERIFY(service_->setContactPolicy(identity_, contact_, "read_profiles", "Deny"));
        auto response = call("bio_probe", {{"identity", identity_}, {"sharing_id", sharing}, {"idempotency_key", retry("probe-denied")}});
        QCOMPARE(response->error, "AccessDenied");
        QCOMPARE(service_->pending().size(), 0);
        QVERIFY(service_->setContactPolicy(identity_, contact_, "read_profiles", "Inherit"));
        QVERIFY(service_->setContactPolicy(identity_, {}, "read_profiles", "Deny"));
        response = call("bio_probe", {{"identity", identity_}, {"sharing_id", sharing}, {"idempotency_key", retry("default-probe-denied")}});
        QCOMPARE(response->error, "AccessDenied");
        const auto unknown = QString::fromLatin1(DsEngine::getIdentityAsBase58(ds::crypto::DsCert::create(), address));
        response = call("bio_probe", {{"identity", identity_}, {"sharing_id", unknown}, {"idempotency_key", retry("unknown-probe-denied")}});
        QCOMPARE(response->error, "AccessDenied");
        response = call("bio_preview", {{"identity", identity_}, {"sharing_id", sharing}, {"idempotency_key", retry("no-preview")}});
        QCOMPARE(response->error, "ProbeChanged");
    }
#endif
    void deny_precedence_policy_changes_and_timeout() {
        service_->setUiAvailable(true);
        auto response = call("message_send", send("deny"));
        QVERIFY(!response->ready);
        QCOMPARE(service_->pending().size(), 1);
        QVERIFY(service_->decide(pendingId(), "Deny", "Selected contacts"));
        QVERIFY(response->ready);
        QCOMPARE(messages(), 0);
        QVERIFY(service_->setRule(key_, identity_, "message_send", {}, "Allow"));
        QVERIFY(service_->setRule(key_, identity_, "message_send", contact_, "Deny"));
        response = call("message_send", send("conflict"));
        QCOMPARE(response->error, "AccessDenied");
        QCOMPARE(messages(), 0);
        for (const auto& row : service_->rules())
            QVERIFY(service_->removeRule(row.toMap().value("id").toString()));
        response = call("message_send", send("invalidate"));
        QVERIFY(!response->ready);
        const auto id = pendingId();
        QVERIFY(service_->setContactPolicy(identity_, contact_, "send_messages", "Ask"));
        QVERIFY(response->ready);
        QCOMPARE(response->value.value("state").toString(), "Cancelled");
        QVERIFY(!service_->decide(id, "Allow once", "Selected contacts"));
        QCOMPARE(messages(), 0);
        response = call("message_send", send("expire"));
        QTRY_VERIFY_WITH_TIMEOUT(response->ready, 2500);
        QCOMPARE(response->value.value("state").toString(), "Expired");
        QCOMPARE(messages(), 0);
    }
    void exactly_one_local_effect_retry_conflict_and_revocation() {
        service_->setUiAvailable(true);
        const auto arguments = send("send-once");
        auto response = call("message_send", arguments);
        QVERIFY(service_->decide(pendingId(), "Allow once", "Selected contacts"));
        QTRY_VERIFY(response->ready);
        QCOMPARE(response->value.value("state").toString(), "Completed");
        QCOMPARE(messages(), 1);
        const auto operation = response->value.value("operation").toString();
        response = call("message_send", arguments);
        QVERIFY(response->ready);
        QCOMPARE(response->value.value("operation").toString(), operation);
        QCOMPARE(messages(), 1);
        auto changed = arguments;
        changed["text"] = "changed";
        response = call("message_send", changed);
        QCOMPARE(response->error, "IdempotencyConflict");
        QCOMPARE(messages(), 1);
        response = call("profile", profile("revoked"));
        QVERIFY(!response->ready);
        QVERIFY(service_->revokeKey(key_));
        QVERIFY(response->ready);
        QCOMPARE(response->value.value("state").toString(), "Cancelled");
        response = call("operation_status", {{"operation", operation}});
        QCOMPARE(response->error, "AccessDenied");
    }
    void disconnected_waiter_and_sensitive_retry_redaction() {
        service_->setUiAvailable(true);
        auto response = call("profile", profile("disconnect"));
        const auto id = pendingId();
        response.reset();
        QVERIFY(service_->decide(id, "Allow once", "Selected contacts"));
        QTRY_VERIFY(service_->pending().isEmpty());
        response = call("profile", profile("disconnect"));
        QVERIFY(response->ready);
        QCOMPARE(response->value.value("operation").toString(), id);
        QVERIFY(!response->value.contains("bio"));
        QVERIFY(!response->value.contains("arguments"));
        const auto history = QJsonDocument::fromVariant(service_->history()).toJson();
        QVERIFY(!history.contains("private fixture biography"));
        auto event = call("events", {{"identity", identity_}});
        QVERIFY(event->ready);
        QVERIFY(!QJsonDocument(event->value).toJson().contains("private fixture biography"));
    }
    void multi_contact_decisions_are_separate_and_contact_ask_wins() {
        service_->setUiAvailable(true);
        auto data = std::make_unique<ContactData>();
        data->identity = engine_->getIdentityManager()->identityFromUuid(QUuid(identity_))->getId();
        data->name = "Second peer";
        data->cert = ds::crypto::DsCert::create();
        data->address = "fixture-not-networked";
        data->autoConnect = false;
        data->state = Contact::ACCEPTED;
        const auto second = engine_->getContactManager()
                                ->addContact(std::move(data))
                                ->getUuid()
                                .toString(QUuid::WithoutBraces);
        auto args = send("multi");
        args["contacts"] = QJsonArray{second, contact_};
        auto response = call("message_send", args);
        QCOMPARE(service_->pending().size(), 1);
        QVERIFY(
            service_->pending().first().toMap().value("arguments").toString().contains("fixture message"));
        QVERIFY(service_->decide(pendingId(), "Allow always", "Selected contacts"));
        QTRY_VERIFY(response->ready);
        QCOMPARE(messages(), 2);
        const auto rules = service_->rules();
        QCOMPARE(rules.size(), 2);
        for (const auto& row : rules)
            QVERIFY(!row.toMap().value("contact").toString().isEmpty());
        QVERIFY(service_->setContactPolicy(identity_, second, "send_messages", "Ask"));
        args["idempotency_key"] = retry("multi-ask");
        response = call("message_send", args);
        QVERIFY(!response->ready);
        QVERIFY(service_->decide(pendingId(), "Deny", "Selected contacts"));
        QCOMPARE(messages(), 2);
        service_->setUiAvailable(false);
        args["idempotency_key"] = retry("multi-no-ui");
        response = call("message_send", args);
        QCOMPARE(response->error, "ApprovalUnavailable");
        QCOMPARE(messages(), 2);
    }
    void durable_recovery_and_expired_epoch_never_reexecute() {
        QVERIFY(service_->setRule(key_, identity_, "message_send", {}, "Allow"));
        const auto args = send("recovery");
        auto response = call("message_send", args);
        QTRY_VERIFY(response->ready);
        QCOMPARE(messages(), 1);
        const auto operation = response->value.value("operation").toString();
        QSqlQuery uncertain;
        uncertain.prepare("UPDATE mcp_operations SET state='Executing' WHERE id=?");
        uncertain.addBindValue(operation);
        QVERIFY(uncertain.exec());
        service_.reset();
        service_ = std::make_unique<Service>(*engine_);
        QVERIFY(service_->configure(true, "127.0.0.1", port_, 60));
        response = call("message_send", args);
        QCOMPARE(response->value.value("operation").toString(), operation);
        QCOMPARE(response->value.value("state").toString(), "IndeterminateOperation");
        QCOMPARE(messages(), 1);
        QVERIFY(!response->value.value("reference").toString().isEmpty());
        // Retire the retry epoch, then remove only this test's terminal evidence
        // to represent expiry/pruning. The old token cannot become new work.
        QSqlQuery expire("UPDATE mcp_retry_epoch SET created=0");
        QVERIFY(expire.lastError().type() == QSqlError::NoError);
        auto fresh = call("status", {{"identity", identity_}});
        QVERIFY(fresh->ready);
        QSqlQuery remove;
        remove.prepare("DELETE FROM mcp_operations WHERE id=?");
        remove.addBindValue(operation);
        QVERIFY(remove.exec());
        response = call("message_send", args);
        QCOMPARE(response->error, "IdempotencyExpired");
        QCOMPARE(messages(), 1);
    }
    void bounded_admission_and_authorization_bound_cursors() {
        service_->setUiAvailable(true);
        QVERIFY(service_->configure(true, "127.0.0.1", port_, 60));
        QList<std::shared_ptr<Reply>> waiting;
        for (int i = 0; i < 4; ++i) {
            auto response = call("profile", profile(QString::number(i)));
            QVERIFY(!response->ready);
            waiting << response;
        }
        auto rejected = call("profile", profile("overload"));
        QCOMPARE(rejected->error, "OperationLimit");
        QCOMPARE(service_->pending().size(), 4);
        auto page = call("contacts", {{"identity", identity_}, {"limit", 1}});
        QVERIFY(page->ready);
        QCOMPARE(page->value.value("cursor").toString().size(), 64);
        const auto cursor = page->value.value("cursor").toString();
        auto other = service_->createKey("Other fixture key", {identity_},
                                         {"read_contacts", "read_events", "read_status"});
        QVERIFY(!other.isEmpty());
        const auto original = key_;
        key_ = other.value("id").toString();
        auto invalid = call("contacts", {{"identity", identity_}, {"cursor", cursor}});
        QCOMPARE(invalid->error, "InvalidCursor");
        auto events = call("events", {{"identity", identity_}});
        QVERIFY(events->ready);
        for (const auto& item : events->value.value("items").toArray()) {
            const auto owner = item.toObject().value("key").toString();
            QVERIFY(owner.isEmpty() || owner == key_);
        }
        key_ = original;
        QVERIFY(service_->decide(pendingId(), "Deny", "Selected contacts"));
    }
    void actual_http_disconnect_preserves_pending_operation() {
        service_->setUiAvailable(true);
        QVERIFY(service_->configure(true, "127.0.0.1", port_, 60));
        const auto args = profile("http-disconnect");
        QJsonObject meta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                         {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}};
        QJsonObject message{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", QJsonObject{{"name", "profile"}, {"arguments", args}, {"_meta", meta}}}};
        QNetworkRequest request(QUrl("http://127.0.0.1:" + QString::number(port_) + "/mcp"));
        request.setRawHeader("Authorization", ("Bearer " + secret_).toUtf8());
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("MCP-Protocol-Version", "2026-07-28");
        request.setRawHeader("Mcp-Method", "tools/call");
        request.setRawHeader("Mcp-Name", "profile");
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkAccessManager network;
        auto* http = network.post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
        QTRY_COMPARE_WITH_TIMEOUT(service_->pending().size(), 1, 3000);
        const auto operation = pendingId();
        http->abort();
        http->deleteLater();
        QCOMPARE(service_->pending().size(), 1);
        QVERIFY(service_->decide(operation, "Allow once", "Selected contacts"));
        QTRY_VERIFY([&] {
            auto state = call("operation_status", {{"operation", operation}});
            return state->value.value("state").toString() == "Completed";
        }());
        auto response = call("profile", args);
        QCOMPARE(response->value.value("operation").toString(), operation);
        QVERIFY(!response->value.contains("bio"));
    }
    void contact_decision_snapshots_and_deleted_contact_retries() {
        const QStringList grants{"read_status", "read_contacts", "create_contacts", "remove_contacts"};
        QVERIFY(service_->updateKey(key_, "Fixture key", {identity_}, grants, 0, true));
        service_->setUiAvailable(true);
        ds::models::NotificationsModel notifications(engine_->settings());
        int decisions = 0;
        service_->setContactDecision([&](int id, bool approve) {
            ++decisions;
            return notifications.decideContactRequest(id, approve);
        });
        const auto identity_id = engine_->getIdentityManager()->identityFromUuid(QUuid(identity_))->getId();
        auto peer = ds::crypto::DsCert::create();
        const auto original = DsEngine::toJson({{"nickName", "Original requester"},
                                                {"handle", peer->getB58PubKey()},
                                                {"address", "fixture-not-networked"}});
        QSqlQuery insert;
        insert.prepare("INSERT INTO notification(status,priority,identity,type,timestamp,message,data) "
                       "VALUES(0,0,?,0,?,'',?)");
        insert.addBindValue(identity_id);
        insert.addBindValue(QDateTime::currentDateTime());
        insert.addBindValue(original);
        QVERIFY(insert.exec());
        auto page = call("contact_requests", {{"identity", identity_}});
        QVERIFY(page->ready);
        const auto handle =
            page->value.value("items").toArray().first().toObject().value("request").toString();
        QCOMPARE(handle.size(), 64);
        const QJsonObject args{
            {"identity", identity_}, {"request", handle}, {"idempotency_key", retry("request-snapshot")}};
        auto response = call("contact_reject", args);
        QCOMPARE(response->error, "AccessDenied");
        auto updated_grants = grants;
        updated_grants << "reject_contacts";
        QVERIFY(service_->updateKey(key_, "Fixture key", {identity_}, updated_grants, 0, true));
        response = call("contact_reject", args);
        QVERIFY(!response->ready);
        QVERIFY(
            service_->pending().first().toMap().value("arguments").toString().contains("Original requester"));
        QSqlQuery changed;
        changed.prepare("UPDATE notification SET data=? WHERE id=?");
        changed.addBindValue(DsEngine::toJson({{"nickName", "Changed requester"}}));
        changed.addBindValue(insert.lastInsertId());
        QVERIFY(changed.exec());
        QVERIFY(service_->decide(pendingId(), "Allow once", "Selected contacts"));
        QTRY_VERIFY(response->ready);
        QCOMPARE(response->value.value("state").toString(), "Cancelled");
        QCOMPARE(decisions, 0);
        // A fresh, approved rejection uses the very same model entry point as the GUI.
        auto fresh = args;
        fresh["idempotency_key"] = retry("request-reject");
        response = call("contact_reject", fresh);
        QVERIFY(service_->decide(pendingId(), "Allow once", "Selected contacts"));
        QTRY_VERIFY(response->ready);
        QCOMPARE(response->value.value("state").toString(), "Completed");
        QCOMPARE(decisions, 1);
        auto repeated = call("contact_reject", fresh);
        QCOMPARE(repeated->value.value("operation"), response->value.value("operation"));
        QCOMPARE(decisions, 1);
        const QJsonObject removal{
            {"identity", identity_}, {"contact", contact_}, {"idempotency_key", retry("remove")}};
        response = call("contact_remove", removal);
        QVERIFY(service_->decide(pendingId(), "Allow once", "Selected contacts"));
        QTRY_VERIFY(response->ready);
        QCOMPARE(response->value.value("state").toString(), "Completed");
        repeated = call("contact_remove", removal);
        QCOMPARE(repeated->value.value("operation"), response->value.value("operation"));
        QCOMPARE(repeated->value.value("state").toString(), "Completed");
    }
    void event_loss_does_not_reveal_other_keys_activity() {
        const auto other =
            service_->createKey("Other event producer", {identity_}, {"read_status", "read_events"});
        QVERIFY(!other.isEmpty());
        auto initial = call("events", {{"identity", identity_}, {"limit", 64}});
        QVERIFY(initial->ready);
        const auto cursor = initial->value.value("cursor").toString();
        const auto original_key = key_;
        key_ = other.value("id").toString();
        for (int i = 0; i < 2200; ++i)
            call("status", {{"identity", identity_}});
        key_ = original_key;
        auto hidden = call("events", {{"identity", identity_}, {"cursor", cursor}});
        QVERIFY(hidden->error.isEmpty());
        QVERIFY(!hidden->value.value("buffer_loss").toBool());
        const auto latest = hidden->value.value("cursor").toString();
        const auto owner = engine_->getIdentityManager()->identityFromUuid(QUuid(identity_));
        for (int i = 0; i < 2200; ++i)
            engine_->events().append(EventJournal::Message, owner->getId(), 0, EventJournal::Local,
                                     EventJournal::Success);
        auto visible = call("events", {{"identity", identity_}, {"cursor", latest}});
        QVERIFY(visible->error.isEmpty());
        QVERIFY(visible->value.value("buffer_loss").toBool());
        QVERIFY(service_->setContactPolicy(identity_, contact_, "read_profiles", "Deny"));
        auto stale = call("events", {{"identity", identity_}, {"cursor", latest}});
        QCOMPARE(stale->error, "InvalidCursor");
    }
    void approval_escapes_invisible_formatting_controls() {
        service_->setUiAvailable(true);
        auto args = send("bidi-display");
        args["text"] = QStringLiteral("before\u202eafter");
        auto response = call("message_send", args);
        QVERIFY(!response->ready);
        const auto displayed = service_->pending().first().toMap().value("arguments").toString();
        QVERIFY(displayed.contains(QStringLiteral("\\u202e")));
        QVERIFY(!displayed.contains(QChar(0x202e)));
        QVERIFY(service_->decide(pendingId(), "Deny", "Selected contacts"));
        QCOMPARE(messages(), 0);
    }
    void sdk_compatibility() {
        const auto python = qEnvironmentVariable("DARKSPEAK_MCP_TEST_PYTHON");
        if (python.isEmpty())
            QSKIP("Set DARKSPEAK_MCP_TEST_PYTHON to the interpreter with tests/mcp/requirements.txt");
        service_->setUiAvailable(true);
        QTimer approval;
        connect(&approval, &QTimer::timeout, this, [this] {
            const auto requests = service_->pending();
            for (const auto& request : requests)
                service_->decide(request.toMap().value("id").toString(), "Allow once", "Selected contacts");
        });
        approval.start(20);
        QProcess process;
        process.start(python, {QStringLiteral(MCP_CLIENT_SCRIPT)});
        QVERIFY(process.waitForStarted(3000));
        process.write(
            QJsonDocument(QJsonObject{{"secret", secret_},
                                      {"identity", identity_},
                                      {"contact", contact_},
                                      {"url", "http://127.0.0.1:" + QString::number(port_) + "/mcp"}})
                .toJson(QJsonDocument::Compact) +
            "\n");
        process.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 20000);
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
        QVERIFY(process.readAllStandardOutput().contains("SDK compatibility passed"));
    }
    void endpoint_failure_preserves_listener_and_controller_is_absent() {
        QVERIFY(!service_->configure(true, "0.0.0.0", port_, 1));
        QVERIFY(service_->enabled());
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        QVERIFY(!service_->configure(true, "127.0.0.1", occupied.serverPort(), 1));
        QVERIFY(service_->enabled());
        QNetworkAccessManager network;
        QNetworkRequest request(QUrl("http://127.0.0.1:" + QString::number(port_) + "/mcp"));
        request.setRawHeader("Authorization", ("Bearer " + secret_).toUtf8());
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Origin", "https://untrusted.invalid");
        auto* reply = network.post(request, "{}");
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(3000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
        reply->deleteLater();
        for (const auto& tool : tools())
            QVERIFY(!tool.name.contains("test") && !tool.name.contains("controller"));
    }
};
QTEST_MAIN(McpServiceTest)
#include "mcp_service_test.moc"
