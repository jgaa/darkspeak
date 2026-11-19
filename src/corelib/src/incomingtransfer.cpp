#include "ds/incomingtransfer.h"
#include "ds/dsengine.h"
#include <QCryptographicHash>
#include <QSqlQuery>
#include <QScopeGuard>
#include <algorithm>
#include <stdexcept>

namespace ds::core {

IncomingTransfer::IncomingTransfer(File::ptr_t file, PeerConnection::ptr_t peer, Start start, QDeadlineTimer deadline)
    : TransferSession{file->checkpointKey().value(), peer ? peer->getConnectionId() : QUuid{},
                      file->checkpointAttempt() ? file->checkpointAttempt()->id : QUuid{}, file->getId()},
      file_{std::move(file)}, peer_{std::move(peer)}, start_{start}, negotiation_deadline_{deadline} {
    auto contact = file_->getContact();
    if (!contact) throw std::invalid_argument("Incoming session has no contact");
    const auto policy_changed = [this] {
        if (!authorized()) stop(Stop::AuthorizationWithdrawn);
    };
    observers_.push_back(connect(contact, &Contact::blockedChanged, this, policy_changed));
    observers_.push_back(connect(contact, &Contact::stateChanged, this, policy_changed));
    observers_.push_back(connect(contact->getIdentity(), &Identity::securityPreferencesChanged, this, policy_changed));
    if (file_->isThumbnail()) {
        primary_ = DsEngine::instance().getFileManager()->getFileFromId(file_->dataThumbnailFor(), *contact, File::INCOMING);
        if (!primary_) throw std::invalid_argument("Incoming preview has no primary");
        observers_.push_back(connect(primary_.get(), &File::stateChanged, this, policy_changed));
    }
    if (peer_) observers_.push_back(connect(peer_.get(), &PeerConnection::disconnectedFromPeer, this,
        [this](const auto &) { stop(Stop::Disconnected); }));
}
IncomingTransfer::~IncomingTransfer() {
    for (const auto &observer : observers_) disconnect(observer);
}
bool IncomingTransfer::authorized() const {
    const auto contact = file_->getContact();
    return contact && contact->getState() == Contact::ACCEPTED && !contact->iBlocked() && contact->canReceiveFile(*file_);
}
bool IncomingTransfer::current() const {
    return accepting() && authorized() && peer_->isConnected() && peer_->getConnectionId() == connection();
}

QCoro::Task<bool> IncomingTransfer::selectCheckpoint() {
    auto &storage = DsEngine::instance().getFileManager()->storage();
    auto saved = co_await file_->prepareCheckpointProgress(file_->checkpointInterval());
    if (!saved || !current() || saved.value().failure != dsfm::TransferFailureReason::None) co_return false;
    completed_content_ = file_->getState() == File::FS_DONE;
    if (!completed_content_ && start_ != Start::Accept && !file_->storageId().isNull()) {
        auto recovered = co_await file_->recoverPublishedCheckpoint(saved.value(), cancellation(), [this] { return current(); });
        if (!recovered || !current()) co_return false;
        completed_content_ = recovered.value();
        if (completed_content_) {
            saved = co_await storage.transferProgress().load(key());
            if (!saved || !current()) co_return false;
        }
    }
    // A duplicate offer is only a recovery hint, never approval of a partial.
    if (start_ == Start::PublishedRecovery && !completed_content_) co_return false;
    if (saved.value().complete && !completed_content_) co_return false;
    if (!completed_content_) file_->setState(File::FS_TRANSFERRING);
    setPhase(Phase::Negotiating);
    QVector<dsfm::DurableCheckpoint> candidates = saved.value().durable_checkpoints;
    const bool fresh = start_ == Start::Accept && file_->offeredConnection() == connection() && file_->checkpointAttempt();
    if (start_ == Start::Accept && !fresh) {
        candidates = {{0, 0, QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256), false, false}};
    }
    if (completed_content_) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [this](const auto &checkpoint) {
            return checkpoint.offset != 0 && !(checkpoint.final && checkpoint.offset == quint64(file_->getSize()) &&
                                               checkpoint.sha256 == file_->getHash());
        }), candidates.end());
    }
    for (int round = 0; round < 3 && current(); ++round) {
        if (fresh) selected_ = file_->checkpointAttempt();
        else {
            const auto request = peer_->sendCheckpointResume(file_->getFileId(), file_->getHash(), quint64(file_->getSize()),
                                                              file_->checkpointInterval(), candidates, negotiation_deadline_);
            auto event = co_await nextEvent();
            if (!event || !current() || event->kind != Event::Kind::Resume || event->request != request || !event->selection)
                co_return false;
            selected_ = event->selection;
            if (selected_->interval != file_->checkpointInterval() || !candidates.contains(selected_->selected) ||
                (completed_content_ && (!selected_->selected.final || selected_->selected.offset != quint64(file_->getSize()) ||
                                         selected_->selected.sha256 != file_->getHash()))) {
                peer_->close(); co_return false;
            }
        }
        if (!current()) co_return false;
        if (completed_content_) {
            auto checked = co_await file_->selectCompletedCheckpoint(*selected_, cancellation(), [this] { return current(); });
            if (!checked || !current()) co_return false;
        } else {
            // Keep the existing preparation/prefix validation helpers, then take
            // the writer lease out of the model for the active data workflow.
            QString reason;
            if (!DsEngine::instance().getFileManager()->canAcceptIncoming(*file_, reason)) co_return false;
            auto prepared = co_await file_->prepareIncoming(cancellation());
            if (!prepared || !current()) co_return false;
            auto checked = co_await file_->selectIncomingCheckpoint(*selected_);
            if (!current()) co_return false;
            if (!checked) {
                auto suspended = co_await file_->suspendIncoming();
                if (!suspended || fresh || !selected_->selected.offset ||
                    (checked.error().code != dsfm::ErrorCode::IntegrityError && checked.error().code != dsfm::ErrorCode::IncompleteContent))
                    co_return false;
                candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [this, round](const auto &candidate) {
                    return candidate.offset >= selected_->selected.offset || (round == 1 && candidate.offset != 0);
                }), candidates.end());
                if (candidates.isEmpty()) co_return false;
                continue;
            }
            if (!fresh) {
                auto recorded = co_await file_->recordCheckpoint(selected_->id, selected_->selected, dsfm::CheckpointPhase::MutuallyConfirmed);
                if (!recorded || !current()) co_return false;
            }
            writer_ = std::move(file_->writer_);
        }
        bindAttempt(selected_->id);
        last_ = selected_->selected;
        offset_ = last_.offset;
        file_->setChannel(peer_->bindTransfer(*file_, DsEngine::instance().transfers().find(key())));
        if (!current()) co_return false;
        peer_->sendCheckpointOfferResponse(file_->getFileId(), *selected_);
        setDeadline(QDeadlineTimer{QDeadlineTimer::Forever});
        setPhase(Phase::Transferring);
        co_return true;
    }
    co_return false;
}

QCoro::Task<bool> IncomingTransfer::confirm(dsfm::DurableCheckpoint checkpoint) {
    auto recorded = co_await file_->recordCheckpoint(attempt(), checkpoint, dsfm::CheckpointPhase::Proposed);
    if (!recorded || !current()) co_return false;
    setDeadline(QDeadlineTimer{30000});
    peer_->sendFileCheckpoint(file_->getFileId(), file_->getHash(), *selected_, checkpoint, dsfm::CheckpointPhase::Proposed);
    int retries = 0;
    while (current()) {
        auto event = co_await nextEvent(std::chrono::seconds{2});
        if (!event || !current()) co_return false;
        if (event->kind == Event::Kind::Deadline) {
            if (++retries <= 5)
                peer_->sendFileCheckpoint(file_->getFileId(), file_->getHash(), *selected_, checkpoint, dsfm::CheckpointPhase::Proposed);
            continue;
        }
        if (event->kind == Event::Kind::Chunk && event->sequence < sequence_ && event->offset <= offset_ &&
            quint64(event->bytes.size()) <= offset_ - event->offset) {
            chunkConsumed();
            if (!checkpoint.final) peer_->sendCheckpointAck(file_->getFileId(), attempt(), sequence_, offset_, false);
            continue;
        }
        if (event->kind != Event::Kind::Checkpoint || event->checkpoint_phase != dsfm::CheckpointPhase::SenderConfirmed ||
            event->checkpoint != checkpoint) {
            peer_->close(); co_return false;
        }
        auto confirmed = co_await file_->recordCheckpoint(attempt(), checkpoint, dsfm::CheckpointPhase::MutuallyConfirmed);
        if (!confirmed || !current()) co_return false;
        last_ = checkpoint;
        peer_->sendFileCheckpoint(file_->getFileId(), file_->getHash(), *selected_, checkpoint, dsfm::CheckpointPhase::MutuallyConfirmed);
        setDeadline(QDeadlineTimer{QDeadlineTimer::Forever});
        co_return true;
    }
    co_return false;
}

QCoro::Task<TransferSession::Outcome> IncomingTransfer::execute() {
    auto &storage = DsEngine::instance().getFileManager()->storage();
    operation_.emplace(storage.holdOperation());
    file_->setChannel(0);
    if (negotiation_deadline_.isForever()) negotiation_deadline_ = QDeadlineTimer{30000};
    setDeadline(negotiation_deadline_);
    if (!authorized()) { stop(Stop::AuthorizationWithdrawn); co_return interrupted(); }
    if (!current()) co_return interrupted();
    if (!peer_->checkpointInterval() || !file_->checkpointInterval())
        co_return Outcome{Outcome::Kind::Failed, dsfm::TransferPauseReason::None, dsfm::TransferFailureReason::UnsupportedVersion};
    if (!co_await selectCheckpoint()) {
        if (!stopped()) stop(Stop::ResourceUnavailable);
        co_return interrupted();
    }
    dsfm::CheckpointWindow window{quint64(file_->getSize()), selected_->interval, offset_};
    auto checkpoint_sequence = file_->getSize() == 0 ? 0 : selected_->next_sequence;
    while (current()) {
        auto event = co_await nextEvent();
        if (!event || !current()) break;
        // Lost receipts may cause a peer to repeat the preceding confirmation.
        if (event->kind == Event::Kind::Checkpoint && last_ != selected_->selected &&
            event->checkpoint == last_ && event->checkpoint_phase == dsfm::CheckpointPhase::SenderConfirmed) {
            peer_->sendFileCheckpoint(file_->getFileId(), file_->getHash(), *selected_, last_, dsfm::CheckpointPhase::MutuallyConfirmed);
            continue;
        }
        const auto size = quint64(event->bytes.size());
        const auto total = quint64(file_->getSize());
        if (event->kind != Event::Kind::Chunk || event->sequence > sequence_ || event->offset > total || size > total - event->offset) {
            peer_->close(); break;
        }
        if (event->sequence < sequence_) {
            if (event->offset + size > offset_) { peer_->close(); break; }
            chunkConsumed();
            peer_->sendCheckpointAck(file_->getFileId(), attempt(), sequence_, offset_, false);
            continue;
        }
        if (event->offset != offset_ || (!size && (!event->final || offset_ != total)) ||
            (event->final && size != total - offset_) ||
            (!event->final && size != 8192 && (offset_ + size) % selected_->interval != 0) ||
            (completed_content_ && (!event->final || size || offset_ != total)) || !window.admit(offset_, size)) {
            peer_->close(); break;
        }
        final_start_ = offset_;
        if (!completed_content_) {
            auto written = co_await writer_->writeAt(offset_, std::move(event->bytes), cancellation());
            if (!current()) break;
            if (!written || quint64(written.value()) != size) { stop(Stop::ResourceUnavailable); break; }
        }
        ++sequence_;
        offset_ += size;
        chunkConsumed();
        file_->setFlowProgress(offset_);
        if (!event->final) peer_->sendCheckpointAck(file_->getFileId(), attempt(), sequence_, offset_, false);
        if (event->final || offset_ % selected_->interval == 0) {
            QByteArray digest = file_->getHash();
            if (!completed_content_) {
                auto prefix = co_await writer_->hashPrefix(offset_, cancellation());
                if (!prefix || !current()) break;
                digest = std::move(prefix).value();
            }
            const dsfm::DurableCheckpoint checkpoint{checkpoint_sequence, offset_, digest, event->final, false};
            if (!co_await confirm(checkpoint) || !window.confirm(offset_)) break;
            ++checkpoint_sequence;
        }
        if (event->final) {
            if (!completed_content_) {
                if (!current()) break;
                setPhase(Phase::Publishing);
                file_->setState(File::FS_HASHING);
                // commit rehashes the complete logical file and resolves atomic
                // publication even if cancellation races its irreversible step.
                auto committed = co_await writer_->commit(cancellation());
                if (!committed) {
                    file_->setState(File::FS_TRANSFERRING);
                    if (!stopped()) stop(Stop::ResourceUnavailable);
                    co_return interrupted();
                }
                writer_.reset();
                file_->data_->pending_id = {};
                file_->saveStorage();
                auto recorded = co_await storage.transferProgress().complete(key(), attempt(), true, true, true);
                if (!recorded) co_return Outcome{Outcome::Kind::Paused, dsfm::TransferPauseReason::ResourceUnavailable};
                file_->clearCheckpointPause();
                file_->transferComplete();
                completed_content_ = true;
            }
            co_return Outcome{Outcome::Kind::Completed};
        }
    }
    if (!stopped()) stop(Stop::Disconnected);
    co_return interrupted();
}

QCoro::Task<TransferSession::Outcome> IncomingTransfer::finalize(Outcome result) {
    auto release = qScopeGuard([&] {
        if (peer_) peer_->releaseTransfer(*file_, result.kind == Outcome::Kind::Completed && selected_ && sequence_, last_, sequence_, final_start_);
        operation_.reset();
    });
    if (!writer_) writer_ = std::move(file_->writer_);
    if (completed_content_ || file_->getState() == File::FS_DONE) {
        if (result.kind != Outcome::Kind::Completed) co_return result;
    } else {
        auto &storage = DsEngine::instance().getFileManager()->storage();
        auto ready = co_await storage.ready();
        if (!ready) throw std::runtime_error("Transfer journal unavailable during finalization");
        auto saved = co_await storage.transferProgress().create({key(), quint64(file_->getSize()), file_->getHash(),
            file_->checkpointInterval() ? file_->checkpointInterval() : dsfm::default_checkpoint_interval, dsfm::TransferSide::Receiver});
        if (!saved) throw std::runtime_error("Transfer journal identity cannot be settled");
        if (saved.value().complete || (!writer_ && file_->pendingId().isNull() && !file_->storageId().isNull() && last_.final)) {
            auto recovered = co_await file_->recoverPublishedCheckpoint(saved.value(), {}, [] { return true; });
            if (!recovered || !recovered.value()) co_return Outcome{Outcome::Kind::Paused, dsfm::TransferPauseReason::ResourceUnavailable};
            result = {Outcome::Kind::Completed};
        } else {
            const bool discard = result.kind == Outcome::Kind::Cancelled || result.kind == Outcome::Kind::Rejected || stopped() == Stop::RemoteFailure;
            if (saved.value().failure != dsfm::TransferFailureReason::None && saved.value().failure != result.failure)
                result = {Outcome::Kind::Failed, dsfm::TransferPauseReason::None, saved.value().failure};
            const bool terminal = result.kind == Outcome::Kind::Failed || result.kind == Outcome::Kind::Cancelled || result.kind == Outcome::Kind::Rejected;
            if (terminal && saved.value().failure == dsfm::TransferFailureReason::None) {
                // Record the cancellation/failure before reclaiming its partial;
                // a crash cannot turn explicit cancellation into a resumable pause.
                auto failed = co_await storage.transferProgress().fail(key(), result.failure, saved.value().attempt);
                if (!failed) throw std::runtime_error("Transfer failure could not be persisted");
            }
            if (writer_) {
                dsfm::Result<void> settled;
                if (discard) settled = co_await writer_->abort();
                else settled = co_await writer_->suspend();
                if (!settled && !terminal) result = {Outcome::Kind::Paused, dsfm::TransferPauseReason::ResourceUnavailable};
                if (!settled) qWarning() << "Transfer writer cleanup failed, code" << int(settled.error().code);
                writer_.reset();
            } else if (discard && !file_->pendingId().isNull()) {
                auto aborted = co_await storage.manager().abortPending(file_->storageNamespace(), file_->pendingId());
                if (!aborted && aborted.error().code != dsfm::ErrorCode::MissingEntry)
                    qWarning() << "Transfer partial cleanup failed, code" << int(aborted.error().code);
            }
            if (!terminal) {
                auto paused = co_await storage.transferProgress().pause(key(), saved.value().attempt, result.pause);
                if (!paused) throw std::runtime_error("Transfer pause could not be persisted");
            }
            file_->projectTransferOutcome(saved.value().description.interval, terminal ? dsfm::TransferPauseReason::None : result.pause,
                terminal ? result.failure : dsfm::TransferFailureReason::None,
                result.kind == Outcome::Kind::Cancelled ? File::FS_CANCELLED : result.kind == Outcome::Kind::Rejected ? File::FS_REJECTED :
                result.kind == Outcome::Kind::Failed ? File::FS_FAILED : File::FS_PAUSED);
            if (peer_ && peer_->isConnected()) {
                try {
                    if (result.kind == Outcome::Kind::Cancelled) peer_->sendFileCancel(file_->getFileId(), FileCancelReason::UserAction);
                    else if (result.kind == Outcome::Kind::Rejected) peer_->sendFileOfferResponse(file_->getFileId(), FileOfferResponseStatus::Rejected);
                    else if (!terminal && selected_ && file_->getChannel()) peer_->sendFilePause(file_->getFileId(), attempt(), last_, result.pause);
                } catch (...) {}
            }
        }
    }
    if (result.kind == Outcome::Kind::Completed && peer_ && selected_ && sequence_) {
        try { peer_->sendCheckpointAck(file_->getFileId(), attempt(), sequence_, offset_, true); } catch (...) {}
    }
    co_return result;
}

} // namespace ds::core
