#include <QtTest>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "ds/crypto.h"
#include "ds/dsengine.h"
#include "ds/messagesmodel.h"
#include "ds/contactsmodel.h"

class HistoryModelTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        qputenv("DARKSPEAK_PROFILE_ROOT", directory_.path().toUtf8());
        qputenv("XDG_DATA_HOME", directory_.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", directory_.filePath("config").toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", "disposable-history-fixture-only");
        crypto_ = std::make_unique<ds::crypto::Crypto>();
        auto settings = std::make_unique<QSettings>(directory_.filePath("settings.ini"), QSettings::IniFormat);
        settings->setValue("dbpath", directory_.filePath("history.sqlite"));
        engine_ = std::make_unique<ds::core::DsEngine>(std::move(settings));
        ds::core::IdentityData identity;
        identity.uuid = QUuid::createUuid();
        identity.name = QStringLiteral("Disposable local identity");
        identity.cert = ds::crypto::DsCert::create();
        identity.hash = identity.cert->getHash().toByteArray();
        identity.autoConnect = false;
        identity_ = engine_->getIdentityManager()->addIdentity(identity);
        QVERIFY(identity_);
        for (int number = 0; number < 2; ++number) {
            auto contact = std::make_unique<ds::core::ContactData>();
            contact->identity = identity_->getId();
            contact->name = QStringLiteral("Fixture peer %1").arg(number);
            contact->nickName = contact->name;
            const auto certificate = ds::crypto::DsCert::create();
            contact->cert = ds::crypto::DsCert::createFromPubkey(certificate->getSigningPubKey().toByteArray());
            contact->address = "fixture-not-used-for-network";
            contact->autoConnect = false;
            auto *peer = engine_->getContactManager()->addContact(std::move(contact));
            QVERIFY(peer);
            conversations_.push_back(engine_->getConversationManager()->getConversation(peer));
        }
        QVERIFY(engine_->getDb().transaction());
        for (int number = 1; number <= 260; ++number) insertMessage(conversations_[0]->getId(), number);
        insertMessage(conversations_[1]->getId(), 1);
        QVERIFY(engine_->getDb().commit());
        model_ = std::make_unique<ds::models::MessagesModel>(*this);
    }
    void people_initially_load_and_return_after_clearing_search() {
        ds::models::ContactsModel people(*this);
        people.setIdentity(identity_->getUuid());
        QCOMPARE(people.rowCount({}), 2);
        people.setSearch(QStringLiteral("peer 1"));
        QCOMPARE(people.rowCount({}), 1);
        people.setSearch(QStringLiteral("no matching contact"));
        QCOMPARE(people.rowCount({}), 0);
        people.setSearch(QString{});
        QCOMPARE(people.rowCount({}), 2);
        people.setSearch(QStringLiteral("   "));
        QCOMPARE(people.rowCount({}), 2);
    }
    void preview_preferences_are_local_and_disabled_by_default() {
        QVERIFY(!identity_->getPreviewImages());
        QVERIFY(!identity_->getPreviewVideo());
        const auto prefix = QStringLiteral("identity/%1/").arg(identity_->getUuid().toString(QUuid::WithoutBraces));
        identity_->setPreviewImages(true);
        identity_->setPreviewVideo(true);
        QVERIFY(engine_->settings().value(prefix + QStringLiteral("previewImages")).toBool());
        QVERIFY(engine_->settings().value(prefix + QStringLiteral("previewVideo")).toBool());
        identity_->setPreviewImages(false);
        identity_->setPreviewVideo(false);
    }
    void stale_selection_results_are_discarded() {
        model_->setConversation(conversations_[0].get());
        QVERIFY(model_->loading());
        model_->setConversation(conversations_[1].get());
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
        QCOMPARE(model_->rowCount(), 1);
        QCOMPARE(content(0), QString::fromUtf8("Пример"));
        QVERIFY(model_->historyError().isEmpty());
    }
    void drafts_belong_to_the_cpp_conversation_state() {
        model_->setConversation(conversations_[0].get());
        model_->setDraft(QStringLiteral("first draft"));
        model_->setConversation(conversations_[1].get());
        model_->setDraft(QStringLiteral("second draft"));
        model_->setConversation(conversations_[0].get());
        QCOMPARE(model_->draft(), QStringLiteral("first draft"));
        model_->setDraft(QString(70000, QChar('x')));
        QCOMPARE(model_->draft().size(), qsizetype(65536));
        model_->setConversation(conversations_[1].get());
        QCOMPARE(model_->draft(), QStringLiteral("second draft"));
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
    }
    void older_pages_and_current_ascii_policy() {
        model_->setConversation(conversations_[0].get());
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
        QCOMPARE(model_->rowCount(), 100);
        QVERIFY(model_->hasOlder());
        model_->loadOlder();
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
        QCOMPARE(model_->rowCount(), 200);
        model_->loadOlder();
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
        QCOMPARE(model_->rowCount(), 260);
        QVERIFY(!model_->hasOlder());
        QCOMPARE(content(0), QString::fromUtf8("Пример"));
        identity_->setAllowUtf8(false);
        QVERIFY(content(0).contains(QStringLiteral("hidden")));
        identity_->setAllowUtf8(true);
        QCOMPARE(content(0), QString::fromUtf8("Пример"));
    }
    void history_window_is_bounded_and_can_page_back_to_latest() {
        QVERIFY(engine_->getDb().transaction());
        for (int number = 261; number <= 860; ++number) insertMessage(conversations_[0]->getId(), number);
        QVERIFY(engine_->getDb().commit());
        model_->loadLatest();
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
        QCOMPARE(model_->rowCount(), 100);
        for (int page = 0; page < 7; ++page) {
            model_->loadOlder();
            QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
            QVERIFY(model_->rowCount() <= 500);
        }
        QCOMPARE(model_->rowCount(), 500);
        QVERIFY(model_->hasNewer());
        model_->loadNewer();
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
        QCOMPARE(model_->rowCount(), 500);
        model_->loadLatest();
        QTRY_VERIFY_WITH_TIMEOUT(!model_->loading(), 5000);
        QCOMPARE(content(model_->rowCount() - 1), QStringLiteral("message 860"));
        QVERIFY(!model_->hasNewer());
    }
    void cleanupTestCase() {
        model_.reset();
        conversations_.clear();
        engine_.reset();
        crypto_.reset();
    }
private:
    void insertMessage(int conversation, int number) {
        QSqlQuery query(engine_->getDb());
        query.prepare("INSERT INTO message(direction,state,conversation_id,conversation,message_id,composed_time,content,signature,sender,encoding) "
                      "VALUES(1,3,:cid,:conversation,:messageId,'2026-11-11T12:00:00.000',:content,:signature,:sender,1)");
        query.bindValue(":cid", conversation);
        query.bindValue(":conversation", QByteArray(32, 'c'));
        query.bindValue(":messageId", (QByteArray::number(conversation) + ':' + QByteArray::number(number)).rightJustified(32, '0'));
        query.bindValue(":content", number == 1 ? QString::fromUtf8("Пример") : QStringLiteral("message %1").arg(number));
        query.bindValue(":signature", QByteArray(64, 's'));
        query.bindValue(":sender", QByteArray(32, 'p'));
        QVERIFY(query.exec());
    }
    QString content(int row) const { return model_->data(model_->index(row), model_->roleNames().key("content")).toString(); }
    QTemporaryDir directory_;
    std::unique_ptr<ds::crypto::Crypto> crypto_;
    std::unique_ptr<ds::core::DsEngine> engine_;
    ds::core::Identity *identity_ = nullptr;
    std::vector<ds::core::Conversation::ptr_t> conversations_;
    std::unique_ptr<ds::models::MessagesModel> model_;
};
QTEST_GUILESS_MAIN(HistoryModelTest)
#include "ui_history_model_test.moc"
