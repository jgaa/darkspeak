#include "ds/transfersession.h"
#include "ds/transfermanager.h"

#include <QCoroSignal>
#include <QDebug>
#include <QThread>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ds::core {

TransferSession::TransferSession(dsfm::TransferKey key, QUuid connection, QUuid attempt, int local_file)
    : key_{std::move(key)}, connection_{connection}, local_file_{local_file}, attempt_{attempt} {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] { stop(Stop::Deadline); });
}

void TransferSession::notify() {
    if (notification_pending_) return;
    notification_pending_ = true;
    // Never resume the workflow from inside a wire/policy/model callback.
    QMetaObject::invokeMethod(this, [this] {
        notification_pending_ = false;
        emit wakeup();
    }, Qt::QueuedConnection);
}

bool TransferSession::deliver(QUuid connection, QUuid attempt, Event event) {
    if (QThread::currentThread() != thread() || connection != connection_ || attempt != attempt_ || !accepting())
        return false;
    constexpr size_t max_controls = 16, max_chunks = 8;
    constexpr qsizetype max_chunk_bytes = 8192;
    if (event.bytes.size() > max_chunk_bytes || event.candidates.size() > dsfm::maximum_retained_checkpoints ||
        event.checkpoint.sha256.size() > 32 ||
        (event.selection && event.selection->selected.sha256.size() > 32)) {
        stop(Stop::ResourceUnavailable);
        return false;
    }
    for (const auto &candidate : event.candidates) {
        if (candidate.sha256.size() > 32) { stop(Stop::ResourceUnavailable); return false; }
    }
    const bool chunk = event.kind == Event::Kind::Chunk;
    if (event.kind == Event::Kind::Writable) {
        for (const auto &queued : events_)
            if (queued.kind == Event::Kind::Writable) return true;
    }
    const auto controls = std::count_if(events_.begin(), events_.end(), [](const auto &queued) {
        return queued.kind != Event::Kind::Chunk;
    });
    if ((chunk && chunks_ >= max_chunks) || (!chunk && size_t(controls) >= max_controls)) {
        stop(Stop::ResourceUnavailable);
        return false;
    }
    if (chunk) ++chunks_;
    events_.push_back(std::move(event));
    notify();
    return true;
}

void TransferSession::stop(Stop reason, dsfm::TransferPauseReason pause, dsfm::TransferFailureReason failure) {
    if (QThread::currentThread() != thread()) throw std::logic_error("Transfer command on wrong thread");
    if (phase_ == Phase::Finished) return;
    // Commands close admission synchronously; they never write an outcome.
    // Explicit cancellation/revocation must still reach local publication after
    // a transport disconnect, while disconnect alone leaves it running.
    const bool terminal_command = stop_ == Stop::Cancelled || stop_ == Stop::Rejected || stop_ == Stop::RemoteFailure;
    if (!stop_ || (!terminal_command && (reason == Stop::Cancelled || reason == Stop::Rejected ||
                                       reason == Stop::AuthorizationWithdrawn || reason == Stop::RemoteFailure))) {
        stop_ = reason;
        requested_pause_ = pause;
        requested_failure_ = failure;
    }
    if (reason != Stop::Disconnected || phase_ != Phase::Publishing) cancel_.cancel();
    events_.clear();
    chunks_ = 0;
    notify();
}

QCoro::Task<std::optional<TransferSession::Event>> TransferSession::nextEvent(std::chrono::milliseconds timeout) {
    while (events_.empty() && !stop_) {
        const auto signal = co_await qCoro(this, &TransferSession::wakeup, timeout);
        if (!signal && !stop_ && events_.empty()) {
            Event event; event.kind = Event::Kind::Deadline;
            co_return event;
        }
    }
    if (stop_) co_return std::nullopt;
    auto event = std::move(events_.front());
    events_.pop_front();
    co_return event;
}

void TransferSession::chunkConsumed() {
    if (!chunks_) {
        if (stop_) return; // A synchronous stop already released queued admissions.
        throw std::logic_error("No admitted chunk to consume");
    }
    --chunks_;
}

void TransferSession::setPhase(Phase phase) {
    if (phase_ == Phase::Finished || phase_ == Phase::Finalizing)
        throw std::logic_error("Transfer workflow already finalized");
    if (phase == Phase::Publishing && stop_) throw std::logic_error("Stopped transfer cannot begin publication");
    phase_ = phase;
    emit phaseChanged();
}

void TransferSession::bindAttempt(QUuid attempt) {
    if (phase_ != Phase::Preparing && phase_ != Phase::Negotiating)
        throw std::logic_error("Attempt changed after negotiation");
    attempt_ = attempt;
    std::erase_if(events_, [](const auto &event) {
        return event.kind != Event::Kind::Resume && event.kind != Event::Kind::Offered && event.kind != Event::Kind::Writable;
    });
    chunks_ = 0;
}

void TransferSession::setDeadline(QDeadlineTimer deadline) {
    if (deadline.isForever()) { deadline_.stop(); return; }
    if (deadline.hasExpired()) { stop(Stop::Deadline); return; }
    deadline_.start(int(std::min<qint64>(deadline.remainingTime(), std::numeric_limits<int>::max())));
}

void TransferSession::limitDeadline(QDeadlineTimer deadline) {
    if (deadline_.isActive() && deadline_.remainingTime() < deadline.remainingTime()) return;
    setDeadline(deadline);
}

bool TransferSession::pendingDecision() const {
    return std::any_of(events_.begin(), events_.end(), [](const auto &event) {
        return event.kind == Event::Kind::Resume || event.kind == Event::Kind::Ready;
    });
}

void TransferSession::parkAdmission() {
    if (manager_) manager_->park(key_);
}

QCoro::Task<void> TransferSession::acquireAdmission() {
    if (manager_) co_await manager_->acquire(key_);
}

TransferSession::Outcome TransferSession::interrupted() const {
    using Pause = dsfm::TransferPauseReason;
    if (stop_ == Stop::Cancelled)
        return {Outcome::Kind::Cancelled, Pause::None, dsfm::TransferFailureReason::UserAction};
    if (stop_ == Stop::Rejected)
        return {Outcome::Kind::Rejected, Pause::None, dsfm::TransferFailureReason::UserAction};
    if (stop_ == Stop::RemoteFailure) return {Outcome::Kind::Failed, Pause::None, requested_failure_};
    if (stop_ == Stop::Paused) return {Outcome::Kind::Paused, requested_pause_};
    return {Outcome::Kind::Paused,
            stop_ == Stop::AuthorizationWithdrawn ? Pause::AuthorizationWithdrawn :
            stop_ == Stop::ResourceUnavailable ? Pause::ResourceUnavailable : Pause::Disconnected};
}

QCoro::Task<void> TransferSession::run() {
    Outcome result;
    try {
        if (stop_) result = interrupted();
        else result = co_await execute();
    } catch (const std::exception &ex) {
        qWarning() << "Transfer workflow stopped:" << ex.what();
        result = {Outcome::Kind::Paused, dsfm::TransferPauseReason::ResourceUnavailable};
    } catch (...) {
        qWarning() << "Transfer workflow stopped with unknown exception";
        result = {Outcome::Kind::Paused, dsfm::TransferPauseReason::ResourceUnavailable};
    }
    phase_ = Phase::Finalizing;
    emit phaseChanged();
    deadline_.stop();
    events_.clear();
    chunks_ = 0;
    try {
        co_await acquireAdmission();
        if ((result.kind == Outcome::Kind::Paused ||
             (result.kind == Outcome::Kind::Offered && stop_ != Stop::Disconnected)) && stop_) result = interrupted();
        result = co_await finalize(result);
    } catch (...) {
        // No retry controller: preserve storage/journal evidence for restart.
        qWarning() << "Transfer finalization could not settle durable evidence";
        result = {Outcome::Kind::Paused, dsfm::TransferPauseReason::ResourceUnavailable};
    }
    outcome_ = result;
    phase_ = Phase::Finished;
    emit phaseChanged();
    qDebug() << "Transfer attempt finalized, outcome" << int(result.kind);
}

} // namespace ds::core
