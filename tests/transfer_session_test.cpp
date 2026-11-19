#include "ds/transfermanager.h"
#include <QCoroSignal>
#include <QSignalSpy>
#include <QtTest>

using namespace ds::core;

class Gate : public QObject {
    Q_OBJECT
signals:
    void released();
};

class ControlledSession : public TransferSession {
public:
    ControlledSession(dsfm::TransferKey key, QUuid connection = QUuid::createUuid(), QUuid attempt = QUuid::createUuid())
        : TransferSession{key, connection, attempt} {}
    Gate io;
    bool hold_io = false, publishing = false, park = false;
    int entered = 0, finalized = 0, events = 0;
    dsfm::CancellationToken observed_cancel;
    void deadline(QDeadlineTimer value) { setDeadline(value); }
protected:
    QCoro::Task<Outcome> execute() override {
        ++entered;
        if (park) parkAdmission();
        if (publishing) setPhase(Phase::Publishing);
        observed_cancel = cancellation();
        if (hold_io) co_await qCoro(&io, &Gate::released);
        if (publishing && !observed_cancel.cancelled()) co_return Outcome{Outcome::Kind::Completed};
        while (auto event = co_await nextEvent()) {
            ++events;
            if (event->final) co_return Outcome{Outcome::Kind::Completed};
        }
        co_return interrupted();
    }
    QCoro::Task<Outcome> finalize(Outcome outcome) override {
        ++finalized;
        co_return outcome;
    }
};

class TransferSessionTest : public QObject {
    Q_OBJECT
    dsfm::TransferKey key(QUuid transfer = QUuid::createUuid()) {
        return {local_, remote_, transfer, dsfm::TransferSide::Receiver};
    }
    QUuid local_ = QUuid::createUuid(), remote_ = QUuid::createUuid();
private slots:
    void duplicate_admission_and_shared_contact_limit() {
        TransferManager manager;
        auto a = std::make_shared<ControlledSession>(key());
        auto b = std::make_shared<ControlledSession>(key());
        auto c = std::make_shared<ControlledSession>(key());
        QVERIFY(manager.submit(a)); QVERIFY(manager.submit(b)); QVERIFY(manager.submit(c));
        QCOMPARE(manager.active(local_, remote_), size_t{2});
        QTRY_COMPARE(a->entered, 1); QTRY_COMPARE(b->entered, 1); QCOMPARE(c->entered, 0);
        auto duplicate = std::make_shared<ControlledSession>(a->key());
        QVERIFY(!manager.submit(duplicate));
        a->stop(TransferSession::Stop::Cancelled);
        QTRY_COMPARE(c->entered, 1);
        QCOMPARE(a->finalized, 1);
        QCOMPARE(manager.active(local_, remote_), size_t{2});
        b->stop(TransferSession::Stop::Cancelled); c->stop(TransferSession::Stop::Cancelled);
        auto closing = manager.shutdown();
        QTRY_VERIFY(closing.isReady());
        QCOMPARE(manager.active(local_, remote_), size_t{0});
    }
    void old_work_and_sessions_share_reservations() {
        TransferManager manager;
        auto old = manager.reserve(key()); QVERIFY(old);
        auto a = std::make_shared<ControlledSession>(key());
        auto b = std::make_shared<ControlledSession>(key());
        QVERIFY(manager.submit(a)); QVERIFY(manager.submit(b));
        QTRY_COMPARE(a->entered, 1); QCOMPARE(b->entered, 0);
        old.value().reset();
        QTRY_COMPARE(b->entered, 1);
        auto closing = manager.shutdown(); QTRY_VERIFY(closing.isReady());
    }
    void shutdown_finalizes_queued_sessions_without_starting_their_work() {
        TransferManager manager;
        std::vector<std::shared_ptr<ControlledSession>> sessions;
        for (int i = 0; i < 5; ++i) {
            auto session = std::make_shared<ControlledSession>(key());
            QVERIFY(manager.submit(session));
            sessions.push_back(std::move(session));
        }
        auto closing = manager.shutdown();
        QTRY_VERIFY(closing.isReady());
        for (const auto &session : sessions) {
            QCOMPARE(session->entered, 0);
            QCOMPARE(session->finalized, 1);
            QVERIFY(session->outcome());
        }
        QCOMPARE(manager.active(local_, remote_), size_t{0});
    }
    void startup_reconciliation_returns_before_the_attempt_starts() {
        TransferManager manager;
        auto session = std::make_shared<ControlledSession>(key());
        auto recovery = manager.reserve(session->key()); QVERIFY(recovery);
        QVERIFY(manager.submit(session));
        QCOMPARE(session->entered, 0);
        recovery.value().reset();
        QTRY_COMPARE(session->entered, 1);
        auto closing = manager.shutdown(); QTRY_VERIFY(closing.isReady());
    }
    void parked_owners_release_admission_and_reacquire_for_finalization() {
        TransferManager manager;
        std::vector<std::shared_ptr<ControlledSession>> sessions;
        for (int i = 0; i < 4; ++i) {
            auto session = std::make_shared<ControlledSession>(key()); session->park = true;
            QVERIFY(manager.submit(session)); sessions.push_back(std::move(session));
        }
        for (const auto &session : sessions) QTRY_COMPARE(session->entered, 1);
        QCOMPARE(manager.active(local_, remote_), size_t{0});
        auto closing = manager.shutdown(); QTRY_VERIFY(closing.isReady());
        for (const auto &session : sessions) QCOMPARE(session->finalized, 1);
        QCOMPARE(manager.active(local_, remote_), size_t{0});
    }
    void stale_events_do_not_reach_new_attempt() {
        TransferManager manager;
        auto session = std::make_shared<ControlledSession>(key());
        QVERIFY(manager.submit(session));
        TransferSession::Event event; event.final = true;
        QVERIFY(!session->deliver(QUuid::createUuid(), session->attempt(), event));
        QVERIFY(!session->deliver(session->connection(), QUuid::createUuid(), event));
        QCOMPARE(session->events, 0);
        QVERIFY(session->deliver(session->connection(), session->attempt(), event));
        QCOMPARE(session->events, 0); // Wire callback cannot resume execution inline.
        QTRY_VERIFY(session->outcome());
        QCOMPARE(session->events, 1); QCOMPARE(session->finalized, 1);
        auto closing = manager.shutdown(); QTRY_VERIFY(closing.isReady());
    }
    void cancellation_keeps_slot_until_joined_io_returns() {
        TransferManager manager;
        auto session = std::make_shared<ControlledSession>(key()); session->hold_io = true;
        QVERIFY(manager.submit(session));
        QTRY_COMPARE(session->entered, 1);
        session->stop(TransferSession::Stop::Cancelled);
        QVERIFY(session->observed_cancel.cancelled()); QVERIFY(!session->accepting());
        QCOMPARE(manager.active(local_, remote_), size_t{1});
        QCOMPARE(session->finalized, 0);
        auto closing = manager.shutdown(); QVERIFY(!closing.isReady());
        emit session->io.released();
        QTRY_VERIFY(closing.isReady());
        QCOMPARE(session->finalized, 1);
        QCOMPARE(session->outcome()->kind, TransferSession::Outcome::Kind::Cancelled);
        QCOMPARE(manager.active(local_, remote_), size_t{0});
    }
    void chunk_and_control_queues_are_bounded_while_io_is_pending() {
        for (const bool chunks : {false, true}) {
            TransferManager manager;
            auto session = std::make_shared<ControlledSession>(key()); session->hold_io = true;
            QVERIFY(manager.submit(session));
            QTRY_COMPARE(session->entered, 1);
            TransferSession::Event event;
            event.kind = chunks ? TransferSession::Event::Kind::Chunk : TransferSession::Event::Kind::Ack;
            event.bytes = QByteArray(chunks ? 8192 : 0, 'x');
            const int bound = chunks ? 8 : 16;
            for (int i = 0; i < bound; ++i) QVERIFY(session->deliver(session->connection(), session->attempt(), event));
            QVERIFY(!session->deliver(session->connection(), session->attempt(), event));
            QVERIFY(!session->accepting());
            emit session->io.released();
            QTRY_VERIFY(session->outcome());
            QCOMPARE(session->outcome()->pause, dsfm::TransferPauseReason::ResourceUnavailable);
            QCOMPARE(session->events, 0);
            auto closing = manager.shutdown(); QTRY_VERIFY(closing.isReady());
        }
    }
    void disconnect_preserves_publication_but_explicit_cancel_does_not() {
        for (const bool cancel : {false, true}) {
            TransferManager manager;
            auto session = std::make_shared<ControlledSession>(key());
            session->hold_io = session->publishing = true;
            QVERIFY(manager.submit(session));
            QTRY_COMPARE(session->entered, 1);
            session->stop(TransferSession::Stop::Disconnected);
            QVERIFY(!session->observed_cancel.cancelled());
            if (cancel) session->stop(TransferSession::Stop::Cancelled);
            QCOMPARE(session->observed_cancel.cancelled(), cancel);
            emit session->io.released();
            QTRY_VERIFY(session->outcome());
            QCOMPARE(session->outcome()->kind, cancel ? TransferSession::Outcome::Kind::Cancelled : TransferSession::Outcome::Kind::Completed);
            auto closing = manager.shutdown(); QTRY_VERIFY(closing.isReady());
        }
    }
    void deadline_wakes_owner_and_finishes_once() {
        TransferManager manager;
        auto session = std::make_shared<ControlledSession>(key());
        QVERIFY(manager.submit(session));
        session->deadline(QDeadlineTimer{0});
        QTRY_VERIFY(session->outcome());
        session->stop(TransferSession::Stop::Cancelled);
        QCOMPARE(session->finalized, 1);
        QCOMPARE(session->outcome()->kind, TransferSession::Outcome::Kind::Paused);
        auto closing = manager.shutdown(); QTRY_VERIFY(closing.isReady());
    }
};
QTEST_GUILESS_MAIN(TransferSessionTest)
#include "transfer_session_test.moc"
