#include <dsfilemanager/transfercheckpoint.h>
#include <dsfilemanager/transferverifier.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <iostream>
#include <stdexcept>
#include <source_location>
using namespace dsfm;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__))                                                                                            \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #__VA_ARGS__);       \
    } while (false)
template <class T> T get(QCoro::Task<Result<T>> task, std::source_location at = std::source_location::current()) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result)
        throw std::runtime_error(std::to_string(at.line()) + " error=" + std::to_string(int(result.error().code)));
    return std::move(result).value();
}
void get(QCoro::Task<Result<void>> task, std::source_location at = std::source_location::current()) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result)
        throw std::runtime_error(std::to_string(at.line()) + " error=" + std::to_string(int(result.error().code)));
}
template <class T> void error(QCoro::Task<Result<T>> task, ErrorCode expected) {
    auto r = QCoro::waitFor(std::move(task));
    CHECK(!r && r.error().code == expected);
}
QByteArray digest(QByteArray bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
TransferDescription description(quint64 size) {
    return {{QUuid::createUuid(), QUuid::createUuid(), QUuid::createUuid()},
            size,
            digest("source"),
            default_checkpoint_interval};
}
void prefixHashing() {
    TransferVerifier verifier(6, digest("abcdef"));
    CHECK(get(verifier.prefix()).sha256 == digest({}));
    get(verifier.add(0, "abc"));
    auto prefix = get(verifier.prefix());
    CHECK(prefix.size == 3 && prefix.sha256 == digest("abc"));
    CHECK(get(verifier.prefix()).sha256 == prefix.sha256);
    get(verifier.add(3, "def"));
    get(verifier.finish());
    CHECK(get(verifier.prefix()).sha256 == digest("abcdef"));
}
void independentWindows() {
    for (auto interval : {minimum_checkpoint_interval, default_checkpoint_interval, maximum_checkpoint_interval}) {
        CHECK(negotiateCheckpointInterval(interval, maximum_checkpoint_interval).value() == interval);
        CheckpointWindow window(quint64(interval) * 3, interval);
        // Ordinary flow ACKs may recycle the eight slots arbitrarily often;
        // admission still stops after one interval of queued/in-flight payload.
        quint64 offset = 0;
        while (window.allowance()) {
            auto count = std::min<quint64>(8192, window.allowance());
            CHECK(window.admit(offset, count));
            offset += count;
        }
        CHECK(offset == interval && !window.admit(offset, 8192));
        CHECK(!window.confirm(offset + 1));
        CHECK(window.confirm(offset));
        CHECK(window.allowance() == interval && window.admit(offset, 8192));
        CHECK(!window.admit(offset, 8192));
        CHECK(!window.confirm(offset - 1));
    }
    CheckpointWindow resumed(2ULL*default_checkpoint_interval,default_checkpoint_interval,1024);
    CHECK(resumed.allowance()==default_checkpoint_interval-1024);
    CHECK(resumed.admit(1024,resumed.allowance()));CHECK(resumed.allowance()==0);
    CHECK(resumed.confirm(default_checkpoint_interval));CHECK(resumed.allowance()==default_checkpoint_interval);
    CHECK(!negotiateCheckpointInterval(0, default_checkpoint_interval));
    CHECK(!negotiateCheckpointInterval(minimum_checkpoint_interval + 1, default_checkpoint_interval));
    CHECK(!negotiateCheckpointInterval(maximum_checkpoint_interval + minimum_checkpoint_interval,
                                       default_checkpoint_interval));
    CheckpointWindow overflow(std::numeric_limits<quint64>::max(), default_checkpoint_interval);
    CHECK(!overflow.admit(0, 1));
}
void progressAndResume() {
    QTemporaryDir temp;
    auto d = description(default_checkpoint_interval * 2 + 7);
    TransferProgressStore store(temp.path() + "/progress");
    get(store.open());
    auto initial = get(store.create(d));
    CHECK(initial.checkpoints.size() == 1);
    CHECK(get(store.create(d)).description.sha256 == d.sha256);
    auto reflected = d;
    reflected.side = TransferSide::Receiver;
    error(store.create(reflected), ErrorCode::Conflict);
    auto changed = d;
    changed.sha256 = digest("other");
    error(store.create(changed), ErrorCode::Conflict);
    auto attempt = QUuid::createUuid();
    get(store.beginAttempt(d.key, attempt, initial.checkpoints.first()));
    DurableCheckpoint first{1, default_checkpoint_interval, digest("first"), false};
    get(store.record(d.key, attempt, first, CheckpointPhase::Proposed));
    CHECK(get(store.load(d.key)).confirmed_offset == 0);
    get(store.record(d.key, attempt, first, CheckpointPhase::SenderConfirmed));
    CHECK(get(store.load(d.key)).confirmed_offset == 0);
    error(store.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
    auto corrupt = first;
    corrupt.sha256 = digest("malicious");
    error(store.record(d.key, attempt, corrupt, CheckpointPhase::MutuallyConfirmed), ErrorCode::IntegrityError);
    get(store.record(d.key, attempt, first, CheckpointPhase::MutuallyConfirmed));
    get(store.record(d.key, attempt, first, CheckpointPhase::MutuallyConfirmed));
    auto saved = get(store.load(d.key));
    CHECK(saved.confirmed_offset == first.offset && saved.next_checkpoint_sequence == 2);
    CHECK(get(store.selectResume(d.key, {initial.checkpoints.first(), first})) == first);
    auto unavailable = first;
    unavailable.sha256 = digest("incorrect receiver prefix");
    CHECK(get(store.selectResume(d.key, {unavailable, initial.checkpoints.first()})).offset == 0);
    error(store.selectResume(d.key, {unavailable}), ErrorCode::IntegrityError);
    error(store.selectResume(d.key, QVector<DurableCheckpoint>(65, first)), ErrorCode::InvalidArgument);
    error(store.record(d.key, QUuid::createUuid(), first, CheckpointPhase::MutuallyConfirmed), ErrorCode::Conflict);
    get(store.pause(d.key, attempt, TransferPauseReason::SourceUnavailable));
    error(store.record(d.key, attempt, first, CheckpointPhase::MutuallyConfirmed), ErrorCode::Conflict);
    get(store.shutdown());
    TransferProgressStore reopened(temp.path() + "/progress");
    get(reopened.open());
    CHECK(get(reopened.load(d.key)).pause == TransferPauseReason::SourceUnavailable);
    const auto previous_attempt = attempt;
    attempt = QUuid::createUuid();
    get(reopened.beginAttempt(d.key, attempt, first));
    error(reopened.pause(d.key, previous_attempt, TransferPauseReason::Disconnected), ErrorCode::Conflict);
    CHECK(get(reopened.load(d.key)).pause == TransferPauseReason::None);
    DurableCheckpoint second{2, default_checkpoint_interval * 2, digest("second"), false};
    get(reopened.record(d.key, attempt, second, CheckpointPhase::MutuallyConfirmed));
    get(reopened.record(d.key, attempt, first, CheckpointPhase::MutuallyConfirmed));
    CHECK(get(reopened.load(d.key)).confirmed_offset == second.offset);
    DurableCheckpoint final{3, d.size, d.sha256, true};
    get(reopened.record(d.key, attempt, final, CheckpointPhase::SenderConfirmed));
    error(reopened.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
    get(reopened.record(d.key, attempt, final, CheckpointPhase::MutuallyConfirmed));
    error(reopened.complete(d.key, attempt, false, true), ErrorCode::IncompleteContent);
    error(reopened.complete(d.key, attempt, true, false), ErrorCode::IncompleteContent);
    get(reopened.complete(d.key, attempt, true, true));
    get(reopened.complete(d.key, attempt, true, true));
    CHECK(get(reopened.load(d.key)).complete);
    get(reopened.shutdown());
}
void lostConfirmationRecovery() {
    QTemporaryDir temp;
    TransferProgressStore sender(temp.path()+"/sender"),receiver(temp.path()+"/receiver");get(sender.open());get(receiver.open());
    auto description_sender=description(default_checkpoint_interval);
    auto description_receiver=description_sender;description_receiver.key.local_scope=QUuid::createUuid();
    description_receiver.side=TransferSide::Receiver;
    auto sent=get(sender.create(description_sender)),received=get(receiver.create(description_receiver));
    auto attempt=QUuid::createUuid();
    get(sender.beginAttempt(description_sender.key,attempt,sent.checkpoints.first()));
    get(receiver.beginAttempt(description_receiver.key,attempt,received.checkpoints.first()));
    DurableCheckpoint final{1,description_sender.size,description_sender.sha256,true};
    get(receiver.record(description_receiver.key,attempt,final,CheckpointPhase::Proposed));
    get(sender.record(description_sender.key,attempt,final,CheckpointPhase::SenderConfirmed));
    // The sender's confirmation is lost. Only the receiver's durable proposal
    // and sender's accepted prefix survive; neither side has mutual progress.
    CHECK(get(sender.load(description_sender.key)).confirmed_offset==0);
    CHECK(get(receiver.load(description_receiver.key)).confirmed_offset==0);
    auto retained=get(receiver.load(description_receiver.key));
    auto selected=get(sender.selectResume(description_sender.key,retained.durable_checkpoints));CHECK(selected==final);
    auto resumed=QUuid::createUuid();
    get(sender.beginAttempt(description_sender.key,resumed,selected,2));
    get(receiver.beginAttempt(description_receiver.key,resumed,selected,2));
    get(sender.record(description_sender.key,resumed,selected,CheckpointPhase::MutuallyConfirmed));
    get(receiver.record(description_receiver.key,resumed,selected,CheckpointPhase::MutuallyConfirmed));
    error(sender.complete(description_sender.key,resumed,true,false),ErrorCode::IncompleteContent);
    // EOF resumption still exchanges a new final confirmation and ordinary ACK.
    auto eof=final;eof.sequence=2;
    get(sender.record(description_sender.key,resumed,eof,CheckpointPhase::MutuallyConfirmed));
    get(receiver.record(description_receiver.key,resumed,eof,CheckpointPhase::MutuallyConfirmed));
    get(sender.complete(description_sender.key,resumed,true,true));
    get(receiver.complete(description_receiver.key,resumed,true,true));
    auto replay=QUuid::createUuid();
    get(receiver.beginAttempt(description_receiver.key,replay,eof,3));
    auto receipt=eof;receipt.sequence=3;
    get(receiver.record(description_receiver.key,replay,receipt,CheckpointPhase::MutuallyConfirmed));
    CHECK(get(receiver.load(description_receiver.key)).complete);
    error(receiver.beginAttempt(description_receiver.key,QUuid::createUuid(),received.checkpoints.first(),4),ErrorCode::Conflict);
    get(sender.shutdown());get(receiver.shutdown());
}
void emptyCompletion() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path());
    get(store.open());
    auto d = description(0);
    d.sha256 = digest({});
    auto initial = get(store.create(d));
    auto attempt = QUuid::createUuid();
    get(store.beginAttempt(d.key, attempt, initial.checkpoints.first()));
    DurableCheckpoint final{0, 0, d.sha256, true};
    error(store.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
    get(store.record(d.key, attempt, final, CheckpointPhase::Proposed));
    error(store.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
    get(store.record(d.key, attempt, final, CheckpointPhase::SenderConfirmed));
    error(store.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
    get(store.record(d.key, attempt, final, CheckpointPhase::MutuallyConfirmed));
    get(store.complete(d.key, attempt, true, true));
    get(store.shutdown());
}
void boundedRetentionAndFailures() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path(), 1);
    get(store.open());
    auto d = description(70ULL * default_checkpoint_interval);
    auto initial = get(store.create(d));
    error(store.create(description(1)), ErrorCode::QuotaExceeded);
    auto attempt = QUuid::createUuid();
    get(store.beginAttempt(d.key, attempt, initial.checkpoints.first()));
    for (quint64 n = 1; n < 70; ++n)
        get(store.record(d.key, attempt, {n, n * d.interval, digest(QByteArray::number(n)), false},
                         CheckpointPhase::MutuallyConfirmed));
    auto saved = get(store.load(d.key));
    CHECK(saved.checkpoints.size() == 64 && saved.checkpoints.first().offset == 0);
    CHECK(saved.checkpoints[1].sequence == 7);
    get(store.fail(d.key, TransferFailureReason::ReceiverCheckpointMismatch));
    get(store.fail(d.key, TransferFailureReason::ReceiverCheckpointMismatch));
    error(store.fail(d.key, TransferFailureReason::SourceIntegrityMismatch), ErrorCode::Conflict);
    CHECK(get(store.load(d.key)).failure == TransferFailureReason::ReceiverCheckpointMismatch);
    get(store.shutdown());
}
void durableCrashBoundary() {
    for (const auto &point :
         {QString("before_transfer_checkpoint_commit"), QString("after_transfer_checkpoint_commit")}) {
        QTemporaryDir temp;
        auto d = description(default_checkpoint_interval * 2);
        {
            TransferProgressStore store(temp.path());
            get(store.open());
            auto state = get(store.create(d));
            get(store.beginAttempt(d.key, QUuid::createUuid(), state.checkpoints.first()));
            get(store.shutdown());
        }
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    {"--crash", temp.path(), d.key.local_scope.toString(), d.key.remote_scope.toString(),
                     d.key.transfer.toString(), point});
        CHECK(child.waitForStarted(5000));
        CHECK(child.waitForFinished(20000));
        if (child.exitCode() != 86)
            throw std::runtime_error(child.readAll().toStdString());
        TransferProgressStore store(temp.path());
        get(store.open());
        auto saved = get(store.load(d.key));
        CHECK(saved.confirmed_offset == (point.startsWith("after") ? default_checkpoint_interval : 0));
        CHECK(saved.checkpoints.size() == (point.startsWith("after") ? 2 : 1));
        get(store.shutdown());
    }
}
void schemaFixture() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path());
    get(store.open());
    get(store.shutdown());
    QFile fixture(QStringLiteral(DSFM_TEST_SOURCE_DIR) + "/fixtures/transfer-progress-v1.json");
    CHECK(fixture.open(QIODevice::ReadOnly));
    const auto expected = QJsonDocument::fromJson(fixture.readAll()).object();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "checkpoint-fixture");
        db.setDatabaseName(temp.path() + "/catalog.db");
        CHECK(db.open());
        QSqlQuery q(db);
        CHECK(q.exec("SELECT version FROM meta"));
        CHECK(q.next());
        CHECK(q.value(0).toInt() == expected["version"].toInt());
        const auto tables = expected["tables"].toObject();
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            CHECK(q.exec("PRAGMA table_info(" + it.key() + ')'));
            QJsonArray actual;
            while (q.next())
                actual.append(q.value(1).toString());
            CHECK(actual == it.value().toArray());
        }
        CHECK(expected["minimumInterval"].toInt() == minimum_checkpoint_interval);
        CHECK(expected["defaultInterval"].toInt() == default_checkpoint_interval);
        CHECK(expected["maximumInterval"].toInt() == maximum_checkpoint_interval);
        db.close();
    }
    QSqlDatabase::removeDatabase("checkpoint-fixture");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().value(1) == "--crash") {
            auto args = app.arguments();
            TransferProgressStore store(args[2]);
            get(store.open());
            TransferKey key{QUuid(args[3]), QUuid(args[4]), QUuid(args[5])};
            auto saved = get(store.load(key));
            qputenv("DSFM_CRASH_AT", args[6].toLatin1());
            get(store.record(key, saved.attempt, {1, default_checkpoint_interval, digest("prefix"), false},
                             CheckpointPhase::MutuallyConfirmed));
            return 42;
        }
        prefixHashing();
        schemaFixture();
        durableCrashBoundary();
        independentWindows();
        progressAndResume();
        lostConfirmationRecovery();
        emptyCompletion();
        boundedRetentionAndFailures();
        std::cout << "checkpoint tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
