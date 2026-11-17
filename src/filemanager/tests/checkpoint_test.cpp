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
#include <QDir>
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
    error(store.create(reflected), ErrorCode::InvalidArgument);
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
    description_receiver.side=TransferSide::Receiver;description_receiver.key.side=TransferSide::Receiver;
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
    error(receiver.validateAttempt(description_receiver.key, {resumed, description_receiver.interval, 1, selected}),
          ErrorCode::InvalidArgument);
    error(receiver.beginAttempt(description_receiver.key, resumed, selected, 1), ErrorCode::InvalidArgument);
    get(sender.beginAttempt(description_sender.key,resumed,selected,2));
    get(receiver.beginAttempt(description_receiver.key,resumed,selected,2));
    CHECK(get(sender.load(description_sender.key)).confirmed_offset == 0);
    CHECK(get(receiver.load(description_receiver.key)).confirmed_offset == 0);
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
void unstartedPauseCannotTouchAStartedAttempt() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path()); get(store.open());
    const auto d = description(6); const auto initial = get(store.create(d));
    get(store.pause(d.key, {}, TransferPauseReason::ResourceUnavailable));
    CHECK(get(store.load(d.key)).pause == TransferPauseReason::ResourceUnavailable);
    const auto attempt = QUuid::createUuid();
    get(store.beginAttempt(d.key, attempt, initial.checkpoints.first()));
    error(store.pause(d.key, {}, TransferPauseReason::Disconnected), ErrorCode::Conflict);
    CHECK(get(store.load(d.key)).pause == TransferPauseReason::None);
    get(store.shutdown());
}
void verifiedReceiverPublicationWinsPause() {
    for (const auto side : {TransferSide::Sender, TransferSide::Receiver}) {
        QTemporaryDir temp;
        TransferProgressStore store(temp.path()); get(store.open());
        auto d = description(6); d.side = side; d.key.side = side;
        const auto initial = get(store.create(d)); const auto attempt = QUuid::createUuid();
        get(store.beginAttempt(d.key, attempt, initial.checkpoints.first()));
        const DurableCheckpoint final{1, 6, d.sha256, true};
        get(store.record(d.key, attempt, final, CheckpointPhase::MutuallyConfirmed));
        get(store.pause(d.key, attempt, TransferPauseReason::AuthorizationWithdrawn));
        error(store.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
        error(store.complete(d.key, attempt, false, true, true), ErrorCode::IncompleteContent);
        error(store.complete(d.key, attempt, true, false, true), ErrorCode::IncompleteContent);
        if (side == TransferSide::Receiver) {
            get(store.complete(d.key, attempt, true, true, true));
            const auto saved = get(store.load(d.key));
            CHECK(saved.complete && saved.pause == TransferPauseReason::None);
        } else error(store.complete(d.key, attempt, true, true, true), ErrorCode::IncompleteContent);
        get(store.shutdown());
    }
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
    CHECK(get(store.load(d.key)).accepted_checkpoints.contains(final));
    CHECK(!get(store.load(d.key)).checkpoints.contains(final));
    CHECK(get(store.selectResume(d.key, {final})) == final);
    error(store.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
    get(store.record(d.key, attempt, final, CheckpointPhase::MutuallyConfirmed));
    get(store.complete(d.key, attempt, true, true));
    get(store.shutdown());
}
void emptyFinalAcceptanceSurvivesReopen() {
    QTemporaryDir temp;
    auto d = description(0); d.sha256 = digest({});
    const DurableCheckpoint final{0, 0, d.sha256, true};
    {
        TransferProgressStore store(temp.path()); get(store.open());
        auto saved = get(store.create(d));
        error(store.selectResume(d.key, {final}), ErrorCode::IntegrityError);
        const auto attempt = QUuid::createUuid();
        get(store.beginAttempt(d.key, attempt, saved.checkpoints.first()));
        get(store.record(d.key, attempt, final, CheckpointPhase::SenderConfirmed));
        get(store.shutdown());
    }
    TransferProgressStore reopened(temp.path()); get(reopened.open());
    const auto saved = get(reopened.load(d.key));
    CHECK(saved.accepted_checkpoints.contains(final));
    CHECK(!saved.checkpoints.contains(final) && !saved.complete);
    CHECK(get(reopened.selectResume(d.key, {final})) == final);
    const auto attempt = QUuid::createUuid();
    get(reopened.beginAttempt(d.key, attempt, final, 1));
    error(reopened.complete(d.key, attempt, true, true), ErrorCode::IncompleteContent);
    get(reopened.record(d.key, attempt, final, CheckpointPhase::MutuallyConfirmed));
    error(reopened.complete(d.key, attempt, true, false), ErrorCode::IncompleteContent);
    get(reopened.complete(d.key, attempt, true, true));
    get(reopened.shutdown());
}
void resumeCandidatesPreserveCheckpointIdentity() {
    for (const auto side : {TransferSide::Sender, TransferSide::Receiver}) {
        QTemporaryDir temp;
        TransferProgressStore store(temp.path());
        get(store.open());
        auto d = description(2ULL * default_checkpoint_interval);
        d.side = side;
        d.key.side = side;
        auto initial = get(store.create(d));
        const auto zero = initial.checkpoints.first();
        const auto attempt = QUuid::createUuid();
        get(store.beginAttempt(d.key, attempt, zero));
        const DurableCheckpoint prefix{1, d.interval, digest("prefix"), false};
        get(store.record(d.key, attempt, prefix, CheckpointPhase::MutuallyConfirmed));

        // A peer cannot turn a regular boundary into an agreed deliberate pause.
        auto forged = prefix;
        forged.pause_boundary = true;
        error(store.selectResume(d.key, {forged}), ErrorCode::IntegrityError);
        CHECK(get(store.selectResume(d.key, {zero, forged})) == zero);
        CHECK(get(store.selectResume(d.key, {zero, prefix})) == prefix);

        const DurableCheckpoint final{2, d.size, d.sha256, true};
        get(store.record(d.key, attempt, final, CheckpointPhase::Proposed));
        // A receiver proposal has no authority on its own at either endpoint.
        CHECK(get(store.selectResume(d.key, {zero, prefix, final})) == prefix);
        get(store.record(d.key, attempt, final, CheckpointPhase::SenderConfirmed));
        CHECK(get(store.selectResume(d.key, {zero, prefix, final})) ==
              (side == TransferSide::Sender ? final : prefix));
        get(store.record(d.key, attempt, final, CheckpointPhase::MutuallyConfirmed));
        auto nonfinal = final;
        nonfinal.final = false;
        error(store.selectResume(d.key, {nonfinal}), ErrorCode::IntegrityError);
        CHECK(get(store.selectResume(d.key, {zero, prefix, nonfinal})) == prefix);
        get(store.complete(d.key, attempt, true, true));
        get(store.shutdown());

        TransferProgressStore reopened(temp.path());
        get(reopened.open());
        // Completion survives restart and allows only a retained final reference.
        error(reopened.selectResume(d.key, {zero, prefix}), ErrorCode::IntegrityError);
        error(reopened.selectResume(d.key, {nonfinal}), ErrorCode::IntegrityError);
        CHECK(get(reopened.selectResume(d.key, {zero, prefix, final})) == final);
        const auto replay = QUuid::createUuid();
        error(reopened.validateAttempt(d.key, {replay, d.interval, 3, prefix}), ErrorCode::Conflict);
        get(reopened.validateAttempt(d.key, {replay, d.interval, 3, final}));
        CHECK(get(reopened.load(d.key)).complete);
        CHECK(get(reopened.load(d.key)).attempt == attempt);
        auto other = d.key;
        other.remote_scope = QUuid::createUuid();
        error(reopened.selectResume(other, {final}), ErrorCode::MissingEntry);
        get(reopened.shutdown());
    }
}
void emptyResumeRequiresFinalReference() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path());
    get(store.open());
    auto d = description(0);
    d.sha256 = digest({});
    const auto initial = get(store.create(d));
    const auto zero = initial.checkpoints.first();
    const auto attempt = QUuid::createUuid();
    get(store.beginAttempt(d.key, attempt, zero));
    auto final = zero;
    final.final = true;
    error(store.selectResume(d.key, {final}), ErrorCode::IntegrityError);
    get(store.record(d.key, attempt, final, CheckpointPhase::MutuallyConfirmed));
    get(store.complete(d.key, attempt, true, true));
    get(store.shutdown());
    TransferProgressStore reopened(temp.path());
    get(reopened.open());
    error(reopened.selectResume(d.key, {zero}), ErrorCode::IntegrityError);
    CHECK(get(reopened.selectResume(d.key, {final})) == final);
    get(reopened.shutdown());
}
void attemptPreflightIsReadOnlyAndRechecked() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path());
    get(store.open());
    auto d = description(2ULL * default_checkpoint_interval);
    auto initial = get(store.create(d));
    const auto original = QUuid::createUuid();
    get(store.beginAttempt(d.key, original, initial.checkpoints.first()));
    const DurableCheckpoint prefix{1, d.interval, digest("prefix"), false};
    get(store.record(d.key, original, prefix, CheckpointPhase::MutuallyConfirmed));
    get(store.pause(d.key, original, TransferPauseReason::Disconnected));
    const TransferAttempt replacement{QUuid::createUuid(), d.interval, 2, prefix};
    get(store.validateAttempt(d.key, replacement));
    auto saved = get(store.load(d.key));
    CHECK(saved.attempt == original && saved.pause == TransferPauseReason::Disconnected);
    CHECK(saved.confirmed_offset == prefix.offset && saved.next_checkpoint_sequence == 2);
    auto bad = replacement;
    bad.interval = minimum_checkpoint_interval;
    error(store.validateAttempt(d.key, bad), ErrorCode::InvalidArgument);
    bad = replacement;
    bad.next_sequence = 1;
    error(store.validateAttempt(d.key, bad), ErrorCode::InvalidArgument);
    bad = replacement;
    bad.selected.pause_boundary = true;
    error(store.validateAttempt(d.key, bad), ErrorCode::IntegrityError);
    // Successful preflight grants no permission to bypass a later terminal failure.
    get(store.fail(d.key, TransferFailureReason::UserAction));
    error(store.beginAttempt(d.key, replacement.id, replacement.selected, replacement.next_sequence), ErrorCode::Conflict);
    error(store.validateAttempt(d.key, replacement), ErrorCode::Conflict);
    CHECK(get(store.load(d.key)).attempt == original);
    get(store.shutdown());
}
void delayedFailuresAreAttemptBoundAndCancellable() {
    QTemporaryDir dir;
    TransferProgressStore store(dir.path());
    get(store.open());
    const auto d = description(default_checkpoint_interval);
    const auto initial = get(store.create(d));
    const auto old = QUuid::createUuid(), replacement = QUuid::createUuid();
    get(store.beginAttempt(d.key, old, initial.checkpoints.first()));
    get(store.beginAttempt(d.key, replacement, initial.checkpoints.first()));
    error(store.fail(d.key, TransferFailureReason::UnsupportedVersion, old), ErrorCode::Conflict);
    error(store.fail(d.key, TransferFailureReason::UnsupportedVersion, QUuid{}), ErrorCode::Conflict);
    CancellationToken cancel;
    cancel.cancel();
    error(store.fail(d.key, TransferFailureReason::UnsupportedVersion, replacement, cancel), ErrorCode::Cancelled);
    auto saved = get(store.load(d.key));
    CHECK(saved.attempt == replacement && saved.failure == TransferFailureReason::None);
    get(store.fail(d.key, TransferFailureReason::UnsupportedVersion, replacement));
    saved = get(store.load(d.key));
    CHECK(saved.failure == TransferFailureReason::UnsupportedVersion);
    CHECK(saved.confirmed_offset == 0 && saved.verified_prefix == 0 && !saved.complete);
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
void verifiedPrefixSurvivesRewindAndRetention() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path());
    get(store.open());
    const auto d = description(100ULL * default_checkpoint_interval);
    auto initial = get(store.create(d));
    auto attempt = QUuid::createUuid();
    get(store.beginAttempt(d.key, attempt, initial.checkpoints.first()));
    for (quint64 sequence = 1; sequence <= 70; ++sequence)
        get(store.record(d.key, attempt, {sequence, sequence * d.interval, digest(QByteArray::number(sequence)), false},
                         CheckpointPhase::MutuallyConfirmed));
    auto saved = get(store.load(d.key));
    CHECK(saved.verified_prefix == 70ULL * d.interval);
    // Repeatedly recover earlier retained content. Neither an attempt rewind
    // nor compaction of old checkpoint digests can erase verified byte history.
    for (quint64 round = 0; round < 70; ++round) {
        attempt = QUuid::createUuid();
        get(store.beginAttempt(d.key, attempt, initial.checkpoints.first(), 71 + round));
        auto rewound = get(store.load(d.key));
        CHECK(rewound.confirmed_offset == 0 && rewound.verified_prefix == saved.verified_prefix);
        get(store.record(d.key, attempt, {71 + round, d.interval, digest("1"), false},
                         CheckpointPhase::MutuallyConfirmed));
    }
    get(store.shutdown());
    TransferProgressStore reopened(temp.path());
    get(reopened.open());
    saved = get(reopened.load(d.key));
    CHECK(saved.confirmed_offset == d.interval && saved.verified_prefix == 70ULL * d.interval);
    CHECK(saved.durable_checkpoints.size() == maximum_retained_checkpoints);
    for (const auto &checkpoint : saved.durable_checkpoints) CHECK(checkpoint.offset <= d.interval);
    // A byte counter is not a retained digest and cannot authorize resume.
    error(reopened.selectResume(d.key, {{70, 70ULL * d.interval, digest("70"), false}}), ErrorCode::IntegrityError);
    get(reopened.fail(d.key, TransferFailureReason::SourceIntegrityMismatch));
    CHECK(get(reopened.load(d.key)).verified_prefix == 70ULL * d.interval);
    get(reopened.shutdown());
}
void directionScopedKeys() {
    QTemporaryDir temp;
    QFile fixture(QStringLiteral(DSFM_TEST_SOURCE_DIR) + "/fixtures/transfer-progress-v3.json");
    CHECK(fixture.open(QIODevice::ReadOnly));
    const auto expected = QJsonDocument::fromJson(fixture.readAll()).object();
    const auto keys = expected["keyFixture"].toObject();
    auto sender = description(2ULL * default_checkpoint_interval);
    sender.key = {QUuid::fromRfc4122(QByteArray::fromHex(keys["localScope"].toString().toLatin1())),
                  QUuid::fromRfc4122(QByteArray::fromHex(keys["remoteScope"].toString().toLatin1())),
                  QUuid::fromRfc4122(QByteArray::fromHex(keys["transfer"].toString().toLatin1()))};
    auto receiver = sender;
    receiver.key.side = receiver.side = TransferSide::Receiver;
    TransferProgressStore store(temp.path(), 2);
    get(store.open());
    auto sent = get(store.create(sender));
    auto received = get(store.create(receiver));
    CHECK(sent.description.key != received.description.key);
    const auto send_attempt = QUuid::createUuid(), receive_attempt = QUuid::createUuid();
    get(store.beginAttempt(sender.key, send_attempt, sent.checkpoints.first()));
    get(store.beginAttempt(receiver.key, receive_attempt, received.checkpoints.first()));
    const DurableCheckpoint checkpoint{1, sender.interval, digest("prefix"), false};
    get(store.record(sender.key, send_attempt, checkpoint, CheckpointPhase::MutuallyConfirmed));
    error(store.record(receiver.key, send_attempt, checkpoint, CheckpointPhase::MutuallyConfirmed), ErrorCode::Conflict);
    CHECK(get(store.load(receiver.key)).verified_prefix == 0);
    get(store.pause(receiver.key, receive_attempt, TransferPauseReason::SourceUnavailable));
    CHECK(get(store.load(sender.key)).pause == TransferPauseReason::None);
    get(store.fail(sender.key, TransferFailureReason::SourceIntegrityMismatch));
    CHECK(get(store.load(receiver.key)).failure == TransferFailureReason::None);
    get(store.shutdown());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "direction-key-fixture");
        db.setDatabaseName(temp.path() + "/catalog.db"); CHECK(db.open());
        QSqlQuery query(db);
        CHECK(query.exec("SELECT key,side FROM transfers ORDER BY side"));
        CHECK(query.next());
        CHECK(query.value(0).toByteArray().size() == expected["keyBytes"].toInt());
        CHECK(query.value(0).toByteArray().toHex() == keys["senderKey"].toString().toLatin1());
        CHECK(query.next());
        CHECK(query.value(0).toByteArray().toHex() == keys["receiverKey"].toString().toLatin1());
        CHECK(!query.next());
        db.close();
    }
    QSqlDatabase::removeDatabase("direction-key-fixture");
    TransferProgressStore reopened(temp.path()); get(reopened.open());
    CHECK(get(reopened.load(sender.key)).verified_prefix == sender.interval);
    CHECK(get(reopened.load(receiver.key)).verified_prefix == 0);
    CHECK(get(reopened.load(receiver.key)).pause == TransferPauseReason::SourceUnavailable);
    get(reopened.shutdown());
}
void seedEarlierJournal(const QString &root, bool populated, int version = 1) {
    CHECK(QDir().mkpath(root));
    CHECK(QFile::setPermissions(root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-progress-fixture");
        db.setDatabaseName(root + "/catalog.db"); CHECK(db.open());
        QSqlQuery query(db);
        CHECK(query.exec("CREATE TABLE meta(version INTEGER NOT NULL)"));
        CHECK(query.exec(QString("INSERT INTO meta VALUES(%1)").arg(version)));
        CHECK(query.exec("CREATE TABLE transfers(key BLOB PRIMARY KEY,size INTEGER NOT NULL,sha BLOB NOT NULL,interval INTEGER NOT NULL,"
                         "attempt BLOB,confirmed INTEGER NOT NULL DEFAULT 0,complete INTEGER NOT NULL DEFAULT 0,"
                         "pause INTEGER NOT NULL DEFAULT 0,failure INTEGER NOT NULL DEFAULT 0,next_sequence INTEGER NOT NULL DEFAULT 1,"
                         "side INTEGER NOT NULL DEFAULT 0)"));
        CHECK(query.exec("CREATE TABLE checkpoints(key BLOB NOT NULL REFERENCES transfers(key),sequence INTEGER NOT NULL,"
                         "offset INTEGER NOT NULL,sha BLOB NOT NULL,final INTEGER NOT NULL,pause_boundary INTEGER NOT NULL,"
                         "phase INTEGER NOT NULL,PRIMARY KEY(key,sequence))"));
        if (version == 2) CHECK(query.exec("ALTER TABLE transfers ADD COLUMN verified_prefix INTEGER NOT NULL DEFAULT 0"));
        const auto key = QByteArray(48, 'k') + (version == 2 ? QByteArray(1, '\0') : QByteArray{});
        if (populated) {
            query.prepare("INSERT INTO transfers(key,size,sha,interval) VALUES(?,?,?,?)");
            query.addBindValue(key); query.addBindValue(6);
            query.addBindValue(digest("legacy")); query.addBindValue(default_checkpoint_interval); CHECK(query.exec());
            query.prepare("INSERT INTO checkpoints VALUES(?,0,0,?,0,0,2)");
            query.addBindValue(key); query.addBindValue(digest({})); CHECK(query.exec());
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("legacy-progress-fixture");
    CHECK(QFile::setPermissions(root + "/catalog.db", QFile::ReadOwner | QFile::WriteOwner));
}
void earlierJournalsAreNeverSilentlyReinterpreted() {
    for (const int version : {1, 2}) for (const bool populated : {false, true}) {
        QTemporaryDir temp;
        const auto root = temp.path() + "/legacy";
        seedEarlierJournal(root, populated, version);
        QFile sentinel(root + "/unrecognized-content");
        CHECK(sentinel.open(QIODevice::WriteOnly)); CHECK(sentinel.write("preserve") == 8); sentinel.close();
        TransferProgressStore store(root);
        if (populated) error(store.open(), ErrorCode::UnsupportedCapability);
        else {
            get(store.open());
            auto fresh = get(store.create(description(6)));
            CHECK(fresh.verified_prefix == 0 && fresh.confirmed_offset == 0);
        }
        get(store.shutdown());
        CHECK(sentinel.open(QIODevice::ReadOnly)); CHECK(sentinel.readAll() == "preserve"); sentinel.close();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-progress-probe");
            db.setDatabaseName(root + "/catalog.db"); CHECK(db.open());
            QSqlQuery query(db);
            CHECK(query.exec("SELECT version FROM meta")); CHECK(query.next());
            CHECK(query.value(0).toInt() == (populated ? version : int(transfer_progress_format_version)));
            CHECK(query.exec("SELECT length(key),confirmed FROM transfers")); CHECK(query.next());
            CHECK(query.value(0).toInt() == (populated && version == 1 ? 48 : 49)); CHECK(query.value(1).toInt() == 0);
            CHECK(!query.next());
            db.close();
        }
        QSqlDatabase::removeDatabase("legacy-progress-probe");
    }
    QTemporaryDir interrupted;
    const auto root = interrupted.path() + "/legacy";
    seedEarlierJournal(root, false);
    {
        TransferProgressStore store(root);
        qputenv("DSFM_FAIL_AT", "before_empty_transfer_journal_reinitialize_commit");
        error(store.open(), ErrorCode::DiskFull);
        qunsetenv("DSFM_FAIL_AT");
        get(store.shutdown());
    }
    // A failed empty-catalog reinitialization rolls back all DDL and can retry.
    TransferProgressStore retried(root); get(retried.open());
    get(retried.create(description(6))); get(retried.shutdown());
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
        CHECK(saved.verified_prefix == saved.confirmed_offset);
        CHECK(saved.checkpoints.size() == (point.startsWith("after") ? 2 : 1));
        get(store.shutdown());
    }
}
void corruptJournalMetadataFailsClosed() {
    for (const auto &statement : {
             "UPDATE transfers SET interval=4296015872",
             "UPDATE transfers SET attempt=x'01'",
             "UPDATE transfers SET attempt=zeroblob(16)",
             "UPDATE transfers SET next_sequence=0",
             "UPDATE transfers SET next_sequence=1152921504606846977",
             "UPDATE transfers SET complete=1,verified_prefix=size",
             "UPDATE transfers SET complete=1,verified_prefix=size,confirmed=size,pause=1",
             "UPDATE transfers SET complete=1,verified_prefix=size,confirmed=size",
             "UPDATE checkpoints SET pause_boundary=2",
             "UPDATE transfers SET empty_final_accepted=1",
             "UPDATE transfers SET empty_final_accepted=2"}) {
        QTemporaryDir temp;
        auto d = description(6);
        {
            TransferProgressStore store(temp.path());
            get(store.open()); get(store.create(d)); get(store.shutdown());
        }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "corrupt-progress-fixture");
            db.setDatabaseName(temp.path() + "/catalog.db");
            CHECK(db.open());
            QSqlQuery q(db);
            CHECK(q.exec("PRAGMA ignore_check_constraints=ON"));
            CHECK(q.exec(QString::fromLatin1(statement)));
            db.close();
        }
        QSqlDatabase::removeDatabase("corrupt-progress-fixture");
        TransferProgressStore reopened(temp.path());
        get(reopened.open());
        error(reopened.load(d.key), ErrorCode::Corruption);
        get(reopened.shutdown());
    }
}
void schemaFixture() {
    QTemporaryDir temp;
    TransferProgressStore store(temp.path());
    get(store.open());
    get(store.shutdown());
    QFile fixture(QStringLiteral(DSFM_TEST_SOURCE_DIR) + "/fixtures/transfer-progress-v3.json");
    CHECK(fixture.open(QIODevice::ReadOnly));
    const auto expected = QJsonDocument::fromJson(fixture.readAll()).object();
    CHECK(expected["version"].toInt() == transfer_progress_format_version);
    CHECK(expected["wireVersion"].toInt() == checkpoint_format_version);
    CHECK(expected["failureReasons"].toObject()["RemoteSourceChanged"].toInt() == int(TransferFailureReason::RemoteSourceChanged));
    CHECK(expected["failureReasons"].toObject()["UserAction"].toInt() == int(TransferFailureReason::UserAction));
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
        corruptJournalMetadataFailsClosed();
        durableCrashBoundary();
        independentWindows();
        progressAndResume();
        lostConfirmationRecovery();
        emptyCompletion();
        verifiedReceiverPublicationWinsPause();
        unstartedPauseCannotTouchAStartedAttempt();
        emptyFinalAcceptanceSurvivesReopen();
        resumeCandidatesPreserveCheckpointIdentity();
        emptyResumeRequiresFinalReference();
        attemptPreflightIsReadOnlyAndRechecked();
        delayedFailuresAreAttemptBoundAndCancellable();
        boundedRetentionAndFailures();
        verifiedPrefixSurvivesRewindAndRetention();
        directionScopedKeys();
        earlierJournalsAreNeverSilentlyReinterpreted();
        std::cout << "checkpoint tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
