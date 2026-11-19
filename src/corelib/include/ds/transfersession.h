#pragma once

#include <dsfilemanager/transfercheckpoint.h>
#include <QDeadlineTimer>
#include <QTimer>
#include <QPointer>
#include <deque>

namespace ds::core {
class TransferManager;

// All methods run on the application's Qt thread. Only run()/its awaited
// children advance the workflow; producers can only deliver events or fence it.
class TransferSession : public QObject {
    Q_OBJECT
public:
    enum class Phase { Preparing, Negotiating, AwaitingDecision, Transferring, Publishing, Finalizing, Finished };
    enum class Stop { Cancelled, Rejected, Disconnected, AuthorizationWithdrawn, Deadline, ResourceUnavailable, RemoteFailure, Paused };
    struct Outcome {
        // Offered retires an attempt before receiver approval while preserving
        // the offered UI projection; its journal still records disconnection.
        enum class Kind { Completed, Paused, Cancelled, Failed, Rejected, Offered };
        Kind kind = Kind::Paused;
        dsfm::TransferPauseReason pause = dsfm::TransferPauseReason::None;
        dsfm::TransferFailureReason failure = dsfm::TransferFailureReason::None;
    };
    struct Event {
        enum class Kind { Offered, Ready, Resume, Chunk, Ack, Checkpoint, Pause, Writable, Deadline };
        Kind kind = Kind::Writable;
        quint64 request = 0, sequence = 0, offset = 0;
        QByteArray bytes;
        bool final = false;
        dsfm::DurableCheckpoint checkpoint;
        dsfm::CheckpointPhase checkpoint_phase = dsfm::CheckpointPhase::Proposed;
        std::optional<dsfm::TransferAttempt> selection;
        QVector<dsfm::DurableCheckpoint> candidates;
        dsfm::TransferPauseReason pause = dsfm::TransferPauseReason::None;
        QDeadlineTimer deadline{QDeadlineTimer::Forever};
    };

    TransferSession(dsfm::TransferKey key, QUuid connection, QUuid attempt, int local_file = 0);
    virtual ~TransferSession() = default;
    const dsfm::TransferKey &key() const noexcept { return key_; }
    QUuid connection() const noexcept { return connection_; }
    QUuid attempt() const noexcept { return attempt_; }
    int localFile() const noexcept { return local_file_; }
    Phase phase() const noexcept { return phase_; }
    const std::optional<Outcome> &outcome() const noexcept { return outcome_; }
    bool accepting() const noexcept { return !stop_ && phase_ != Phase::Finalizing && phase_ != Phase::Finished; }
    // Called only after transport validation. Counts/bytes are checked before
    // retaining any peer payload; stale transport/attempt events are rejected.
    bool deliver(QUuid connection, QUuid attempt, Event event);
    void stop(Stop reason, dsfm::TransferPauseReason pause = dsfm::TransferPauseReason::None,
              dsfm::TransferFailureReason failure = dsfm::TransferFailureReason::None);

signals:
    void wakeup();
    void phaseChanged();

protected:
    virtual QCoro::Task<Outcome> execute() = 0;
    // Runs exactly once, after execute and all awaited child work have returned.
    // A definite publication may supersede cancellation here.
    virtual QCoro::Task<Outcome> finalize(Outcome outcome) = 0;
    QCoro::Task<std::optional<Event>> nextEvent(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
    void chunkConsumed();
    void setPhase(Phase phase);
    void bindAttempt(QUuid attempt);
    void setDeadline(QDeadlineTimer deadline);
    void limitDeadline(QDeadlineTimer deadline);
    bool pendingDecision() const;
    void parkAdmission();
    QCoro::Task<void> acquireAdmission();
    dsfm::CancellationToken cancellation() const { return cancel_; }
    std::optional<Stop> stopped() const { return stop_; }
    Outcome interrupted() const;

private:
    friend class TransferManager;
    QCoro::Task<void> run();
    void notify();
    const dsfm::TransferKey key_;
    const QUuid connection_;
    const int local_file_;
    QUuid attempt_;
    Phase phase_ = Phase::Preparing;
    std::optional<Stop> stop_;
    dsfm::TransferPauseReason requested_pause_ = dsfm::TransferPauseReason::None;
    dsfm::TransferFailureReason requested_failure_ = dsfm::TransferFailureReason::None;
    std::optional<Outcome> outcome_;
    dsfm::CancellationToken cancel_;
    std::deque<Event> events_;
    size_t chunks_ = 0;
    QTimer deadline_;
    bool notification_pending_ = false;
    QPointer<TransferManager> manager_;
};

} // namespace ds::core
