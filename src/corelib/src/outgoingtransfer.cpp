#include "ds/outgoingtransfer.h"
#include "ds/dsengine.h"
#include <QCoroSignal>
#include <QSqlQuery>
#include <QScopeGuard>
#include <algorithm>
#include <stdexcept>

namespace ds::core {
namespace { dsfm::Error cancelled() { return dsfm::Error{dsfm::ErrorCode::Cancelled}; } }
OutgoingTransfer::OutgoingTransfer(File::ptr_t file, PeerConnection::ptr_t peer, OutgoingTransferStart start, std::optional<Event> request)
    : TransferSession{file->checkpointKey().value(), peer ? peer->getConnectionId() : QUuid{}, file->checkpointAttempt() ? file->checkpointAttempt()->id : QUuid{}, file->getId()},
      file_{std::move(file)}, peer_{std::move(peer)}, start_{start}, request_{std::move(request)} {
    auto contact = file_->getContact();
    if (!contact) throw std::invalid_argument("Outgoing session has no contact");
    auto policy = [this] { current(); };
    observers_.push_back(connect(contact, &Contact::peerPreferencesChanged, this, policy));
    observers_.push_back(connect(contact, &Contact::blockedChanged, this, policy));
    observers_.push_back(connect(contact, &Contact::stateChanged, this, policy));
    if (peer_) observers_.push_back(connect(peer_.get(), &PeerConnection::disconnectedFromPeer, this, [this](const auto &) { stop(Stop::Disconnected); }));
    if (peer_) observers_.push_back(connect(peer_.get(), &PeerConnection::outputBufferEmptied, this, [this] {
        Event event; event.kind = Event::Kind::Writable;
        deliver(connection(), attempt(), std::move(event));
    }));
}
OutgoingTransfer::~OutgoingTransfer() { for (const auto &observer : observers_) disconnect(observer); }
bool OutgoingTransfer::current() {
    if (!accepting()) return false;
    const auto contact = file_->getContact();
    if (!contact || contact->getState() != Contact::ACCEPTED || !contact->canSendFileType(file_->getMimeType(), file_->getName()) || !file_->outgoingSourceCurrent()) {
        stop(Stop::AuthorizationWithdrawn); return false;
    }
    if (!peer_->isConnected()) { stop(Stop::Disconnected); return false; }
    return true;
}

QCoro::Task<dsfm::Result<void>> OutgoingTransfer::openSource() {
    if (reader_) { auto closed = co_await reader_->close(); reader_.reset(); if (!closed) co_return closed.error(); }
    dsfm::ReadPreparationOptions options; options.expected_sha256 = file_->getHash(); options.cancel = cancellation();
    auto opened = co_await file_->prepareOutgoingRead(options);
    if (!opened) {
        auto error = opened.error();
        // A backend refusal is not locally proved source corruption. Only our
        // ordered digest checks below can establish that terminal outcome.
        if (error.code == dsfm::ErrorCode::IntegrityError) error.code = dsfm::ErrorCode::ExternalModification;
        co_return error;
    }
    auto prepared = std::move(opened).value();
    reader_ = std::move(prepared.reader);
    if (!current()) co_return cancelled();
    if (prepared.info.size != quint64(file_->getSize()) || prepared.info.sha256 != file_->getHash())
        co_return dsfm::Error{dsfm::ErrorCode::ExternalModification};
    verifier_ = std::make_unique<dsfm::TransferVerifier>(prepared.info.size, prepared.info.sha256);
    co_return dsfm::Result<void>{};
}

QCoro::Task<dsfm::Result<void>> OutgoingTransfer::hashBytes(quint64 offset, QByteArray bytes) {
    quint64 position = offset;
    qsizetype consumed = 0;
    do {
        auto end = offset + quint64(bytes.size());
        for (const auto &trusted : trusted_) if (trusted.offset > position) end = std::min(end, trusted.offset);
        const auto length = qsizetype(end - position);
        auto added = co_await verifier_->add(position, bytes.mid(consumed, length), cancellation());
        if (!current()) co_return cancelled();
        if (!added) {
            auto error = added.error();
            if (error.code == dsfm::ErrorCode::IntegrityError) error.code = dsfm::ErrorCode::ExternalModification;
            co_return error;
        }
        for (const auto &trusted : trusted_) {
            if (trusted.offset != end) continue;
            auto prefix = co_await verifier_->prefix(cancellation());
            if (!current()) co_return cancelled();
            if (!prefix) co_return prefix.error();
            if (prefix.value().size != end) co_return dsfm::Error{dsfm::ErrorCode::Corruption};
            if (prefix.value().sha256 != trusted.sha256) co_return dsfm::Error{dsfm::ErrorCode::IntegrityError};
        }
        consumed += length; position = end;
    } while (consumed < bytes.size());
    co_return dsfm::Result<void>{};
}

QCoro::Task<dsfm::Result<void>> OutgoingTransfer::verifyPrefix(dsfm::DurableCheckpoint selected) {
    auto opened = co_await openSource();
    if (!opened) co_return opened.error();
    quint64 offset = 0;
    while (offset < selected.offset) {
        const auto count = qsizetype(std::min<quint64>(8192, selected.offset - offset));
        auto bytes = co_await reader_->readAt(offset, count, cancellation());
        if (!current()) co_return cancelled();
        if (!bytes) co_return bytes.error();
        if (bytes.value().size() != count) co_return dsfm::Error{dsfm::ErrorCode::ExternalModification};
        auto checked = co_await hashBytes(offset, std::move(bytes).value());
        if (!checked) co_return checked.error();
        offset += quint64(count);
    }
    auto prefix = co_await verifier_->prefix(cancellation());
    if (!current()) co_return cancelled();
    if (!prefix) co_return prefix.error();
    if (prefix.value().size != selected.offset || prefix.value().sha256 != selected.sha256)
        co_return dsfm::Error{dsfm::ErrorCode::IntegrityError};
    co_return dsfm::Result<void>{};
}

QCoro::Task<dsfm::Result<void>> OutgoingTransfer::offer() {
    auto &files = *DsEngine::instance().getFileManager();
    auto saved = co_await file_->prepareCheckpointProgress(file_->checkpointInterval() ? file_->checkpointInterval() : peer_->checkpointInterval());
    if (!current()) co_return cancelled();
    if (!saved) co_return saved.error();
    if (saved.value().failure != dsfm::TransferFailureReason::None || saved.value().complete)
        co_return dsfm::Error{dsfm::ErrorCode::Conflict};
    trusted_ = saved.value().accepted_checkpoints;
    const bool untouched = saved.value().verified_prefix == 0 && saved.value().confirmed_offset == 0 &&
        std::none_of(trusted_.begin(), trusted_.end(), [](const auto &checkpoint) { return checkpoint.final || checkpoint.offset; });
    if (start_ == OutgoingTransferStart::Recover && (!untouched || file_->isThumbnail())) {
        if (std::none_of(trusted_.begin(), trusted_.end(), [this](const auto &checkpoint) {
            return checkpoint.final && checkpoint.offset == quint64(file_->getSize()) && checkpoint.sha256 == file_->getHash();
        })) {
            // A partial needs explicit receiver approval. Keep its sole owner
            // awaiting that command, without emitting a recovery offer or I/O.
            bindAttempt(saved.value().attempt);
            setPhase(Phase::AwaitingDecision);
            co_return dsfm::Result<void>{};
        }
        auto opened = co_await openSource();
        if (!opened) co_return opened.error();
        bindAttempt(saved.value().attempt);
        if (file_->isThumbnail()) {
            auto primary = files.getFileFromId(file_->dataThumbnailFor(), *file_->getContact(), File::OUTGOING);
            if (!primary || !primary->outgoingSourceCurrent() || !file_->getContact()->canSendFileType(primary->getMimeType(), primary->getName()))
                co_return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
            auto primary_key = primary->checkpointKey(); if (!primary_key) co_return primary_key.error();
            auto original = co_await files.storage().transferProgress().load(primary_key.value());
            if (!original || !current()) co_return cancelled();
            peer_->repeatCheckpointPreviewOffer(*primary, *file_, original.value().attempt, saved.value().attempt);
        } else peer_->repeatCheckpointOffer(*file_, saved.value().attempt);
        co_return dsfm::Result<void>{};
    }
    if (!untouched || (!saved.value().attempt.isNull() && start_ == OutgoingTransferStart::Offer))
        co_return dsfm::Error{dsfm::ErrorCode::Conflict};
    selected_ = dsfm::TransferAttempt{QUuid::createUuid(), saved.value().description.interval,
                                    saved.value().next_checkpoint_sequence, saved.value().checkpoints.first()};
    auto verified = co_await verifyPrefix(selected_->selected);
    if (!verified) co_return verified.error();
    auto begun = co_await file_->beginOutgoingCheckpointAttempt(*selected_, cancellation());
    if (!begun || !current()) co_return begun;
    bindAttempt(selected_->id);
    file_->setState(File::FS_OFFERED);
    file_->markOutgoingOffer(connection());
    if (start_ == OutgoingTransferStart::ManifestMember) {
        setPhase(Phase::AwaitingDecision);
        co_return dsfm::Result<void>{};
    }
    preview_ = files.thumbnailFor(*file_);
    if (preview_ && file_->getContact()->canSendFileType("image/jpeg", "preview.jpg")) {
        auto preview_key = preview_->checkpointKey(); if (!preview_key) co_return preview_key.error();
        auto &manager = DsEngine::instance().transfers();
        if (!manager.find(preview_key.value())) {
            auto admitted = manager.submit(std::make_shared<OutgoingTransfer>(preview_, peer_, OutgoingTransferStart::ManifestMember));
            if (!admitted) co_return admitted.error();
        }
        // The preview's own root persists its attempt. The primary only waits
        // for that projection before sending the shared signed manifest.
        auto sibling = manager.find(preview_key.value());
        if (reader_) {
            auto closed = co_await reader_->close(); reader_.reset(); verifier_.reset();
            if (!closed) co_return closed.error();
        }
        parkAdmission();
        const auto observer = connect(sibling.get(), &TransferSession::phaseChanged, this, [this] {
            Event event; event.kind = Event::Kind::Writable; deliver(connection(), attempt(), std::move(event));
        });
        while (current() && sibling->phase() != Phase::AwaitingDecision) {
            if (sibling->phase() == Phase::Finished || sibling->phase() == Phase::Finalizing) {
                disconnect(observer); co_return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
            }
            auto event = co_await nextEvent();
            if (!event) { disconnect(observer); co_return cancelled(); }
            if (event->kind == Event::Kind::Resume) request_ = std::move(event);
        }
        disconnect(observer);
        if (!current()) co_return cancelled();
        co_await acquireAdmission();
        if (!current()) co_return cancelled();
        auto opened = co_await openSource();
        if (!opened) co_return opened.error();
    } else preview_.reset();
    try { peer_->offerFile(*file_); }
    catch (...) { stop(Stop::Disconnected); co_return cancelled(); }
    if (preview_) {
        auto sibling = DsEngine::instance().transfers().find(preview_->checkpointKey().value());
        if (sibling) { Event event; event.kind = Event::Kind::Offered; sibling->deliver(connection(), sibling->attempt(), std::move(event)); }
    }
    setPhase(Phase::AwaitingDecision);
    co_return dsfm::Result<void>{};
}

QCoro::Task<dsfm::Result<void>> OutgoingTransfer::negotiate() {
    auto &progress = DsEngine::instance().getFileManager()->storage().transferProgress();
    for (int rounds = 0; current();) {
        std::optional<Event> event;
        if (request_) event = std::exchange(request_, {});
        else event = co_await nextEvent();
        if (!event || !current()) co_return cancelled();
        if (event->kind == Event::Kind::Offered) { setDeadline(QDeadlineTimer{QDeadlineTimer::Forever}); continue; }
        if (event->kind == Event::Kind::Writable) continue;
        if (event->kind == Event::Kind::Ready) {
            if (!selected_ || !event->selection || *selected_ != *event->selection) co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
            setPhase(Phase::Negotiating);
            limitDeadline(event->deadline);
            co_await acquireAdmission();
            if (!current()) co_return cancelled();
            auto recorded = co_await file_->recordCheckpoint(attempt(), selected_->selected, dsfm::CheckpointPhase::MutuallyConfirmed);
            if (!recorded || !current()) co_return recorded;
            // Rebuild from the same source after an arbitrarily long decision wait.
            co_return co_await verifyPrefix(selected_->selected);
        }
        if (event->kind != Event::Kind::Resume || event->candidates.isEmpty() || ++rounds > 3)
            co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        setPhase(Phase::Negotiating);
        // An explicit resume is acceptance, even when the history still shows
        // the original offer. Project that decision before issuing readiness.
        file_->setState(File::FS_TRANSFERRING);
        resume_request_ = event->request;
        limitDeadline(event->deadline);
        co_await acquireAdmission();
        if (!current()) co_return cancelled();
        auto saved = co_await progress.load(key());
        if (!saved || !current()) co_return cancelled();
        if (saved.value().failure != dsfm::TransferFailureReason::None || saved.value().complete ||
            saved.value().description.side != dsfm::TransferSide::Sender ||
            saved.value().description.size != quint64(file_->getSize()) || saved.value().description.sha256 != file_->getHash() ||
            saved.value().description.interval != file_->checkpointInterval()) co_return dsfm::Error{dsfm::ErrorCode::Conflict};
        trusted_ = saved.value().accepted_checkpoints;
        auto selected = co_await progress.selectResume(key(), event->candidates);
        if (!selected || !current()) {
            if (current()) peer_->sendCheckpointResumeResult(event->request, file_->getFileId(), FileResumeStatus::Rejected);
            co_return selected ? cancelled() : selected.error();
        }
        auto checked = co_await verifyPrefix(selected.value());
        if (!checked) co_return checked.error();
        selected_ = dsfm::TransferAttempt{QUuid::createUuid(), saved.value().description.interval,
                                        saved.value().next_checkpoint_sequence, selected.value()};
        auto begun = co_await file_->beginOutgoingCheckpointAttempt(*selected_, cancellation());
        if (!begun || !current()) co_return begun;
        bindAttempt(selected_->id);
        peer_->sendCheckpointResumeResult(event->request, file_->getFileId(), FileResumeStatus::Accepted, selected_);
        resume_request_.reset();
    }
    co_return cancelled();
}

bool OutgoingTransfer::redundant(const Event &event) {
    if (event.kind == Event::Kind::Writable || event.kind == Event::Kind::Offered) return true;
    if (event.kind == Event::Kind::Ready && event.selection && selected_ && *event.selection == *selected_) return true;
    if (event.kind == Event::Kind::Checkpoint && event.checkpoint == last_ && !last_.sha256.isEmpty()) {
        if (event.checkpoint_phase == dsfm::CheckpointPhase::Proposed)
            peer_->sendFileCheckpoint(file_->getFileId(), file_->getHash(), *selected_, last_, dsfm::CheckpointPhase::SenderConfirmed);
        return event.checkpoint_phase != dsfm::CheckpointPhase::SenderConfirmed;
    }
    return false;
}

bool OutgoingTransfer::acknowledge(const Event &event) {
    if (event.kind != Event::Kind::Ack || !event.sequence || event.sequence > next_sequence_ || event.offset > quint64(file_->getSize())) return false;
    if (event.sequence <= ack_sequence_) return event.sequence == ack_sequence_ && event.offset == ack_offset_ && (!event.final || final_ack_);
    auto pending = in_flight_.find(event.sequence);
    if (pending == in_flight_.end() || pending->second != event.offset) return false;
    in_flight_.erase(in_flight_.begin(), std::next(pending));
    ack_sequence_ = event.sequence; ack_offset_ = event.offset;
    file_->setFlowProgress(event.offset);
    if (event.final) {
        if (event.offset != quint64(file_->getSize()) || event.sequence != next_sequence_ || !in_flight_.empty()) return false;
        final_ack_ = true;
    }
    return true;
}

QCoro::Task<dsfm::Result<void>> OutgoingTransfer::confirm(dsfm::DurableCheckpoint checkpoint) {
    // First await the receiver's proposal, processing flow ACKs as events.
    setDeadline(QDeadlineTimer{30000});
    while (current()) {
        auto event = co_await nextEvent();
        if (!event || !current()) co_return cancelled();
        if ((event->kind != Event::Kind::Checkpoint || event->checkpoint != checkpoint) && redundant(*event)) continue;
        if (event->kind == Event::Kind::Ack) { if (!acknowledge(*event)) co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument}; continue; }
        if (event->kind != Event::Kind::Checkpoint || event->checkpoint_phase != dsfm::CheckpointPhase::Proposed)
            co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        if (event->checkpoint != checkpoint) {
            if (event->checkpoint.sequence == checkpoint.sequence && event->checkpoint.offset == checkpoint.offset &&
                event->checkpoint.final == checkpoint.final && !event->checkpoint.pause_boundary)
                co_return dsfm::Error{dsfm::ErrorCode::IntegrityError};
            co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        }
        break;
    }
    if (!current()) co_return cancelled();
    auto recorded = co_await file_->recordCheckpoint(attempt(), checkpoint, dsfm::CheckpointPhase::SenderConfirmed);
    if (!recorded || !current()) co_return recorded;
    peer_->sendFileCheckpoint(file_->getFileId(), file_->getHash(), *selected_, checkpoint, dsfm::CheckpointPhase::SenderConfirmed);
    int retries = 0;
    while (current()) {
        auto event = co_await nextEvent(std::chrono::seconds{2});
        if (!event || !current()) co_return cancelled();
        if ((event->kind != Event::Kind::Checkpoint || event->checkpoint != checkpoint) && redundant(*event)) continue;
        if (event->kind == Event::Kind::Ack) { if (!acknowledge(*event)) co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument}; continue; }
        if (event->kind == Event::Kind::Deadline ||
            (event->kind == Event::Kind::Checkpoint && event->checkpoint == checkpoint && event->checkpoint_phase == dsfm::CheckpointPhase::Proposed)) {
            if (++retries <= 5) peer_->sendFileCheckpoint(file_->getFileId(), file_->getHash(), *selected_, checkpoint, dsfm::CheckpointPhase::SenderConfirmed);
            continue;
        }
        if (event->kind != Event::Kind::Checkpoint || event->checkpoint != checkpoint || event->checkpoint_phase != dsfm::CheckpointPhase::MutuallyConfirmed)
            co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        auto confirmed = co_await file_->recordCheckpoint(attempt(), checkpoint, dsfm::CheckpointPhase::MutuallyConfirmed);
        if (!confirmed || !current()) co_return confirmed;
        last_ = checkpoint;
        co_return dsfm::Result<void>{};
    }
    co_return cancelled();
}

TransferSession::Outcome OutgoingTransfer::sourceError(dsfm::Error error, bool proof) {
    if (stopped() == Stop::Disconnected && phase() == Phase::AwaitingDecision)
        return {Outcome::Kind::Offered, dsfm::TransferPauseReason::Disconnected};
    if (stopped()) return interrupted();
    if (error.code == dsfm::ErrorCode::IntegrityError && proof)
        return {Outcome::Kind::Failed, dsfm::TransferPauseReason::None, dsfm::TransferFailureReason::SourceIntegrityMismatch};
    using Code = dsfm::ErrorCode;
    const auto pause = error.code == Code::MissingEntry || error.code == Code::IoError || error.code == Code::StorageUnavailable
        ? dsfm::TransferPauseReason::SourceUnavailable :
        error.code == Code::ExternalModification || error.code == Code::Busy || error.code == Code::IntegrityError
        ? dsfm::TransferPauseReason::SourceRevalidationRequired :
        error.code == Code::PermissionDenied ? dsfm::TransferPauseReason::AuthorizationWithdrawn : dsfm::TransferPauseReason::ResourceUnavailable;
    return {Outcome::Kind::Paused, pause};
}

QCoro::Task<TransferSession::Outcome> OutgoingTransfer::execute() {
    auto &storage = DsEngine::instance().getFileManager()->storage();
    operation_.emplace(storage.holdOperation());
    file_->setChannel(0);
    setDeadline(QDeadlineTimer{30000});
    if (!current()) co_return interrupted();
    if (!peer_->checkpointInterval()) co_return Outcome{Outcome::Kind::Failed, dsfm::TransferPauseReason::None, dsfm::TransferFailureReason::UnsupportedVersion};
    if (start_ == OutgoingTransferStart::Recover && file_->getState() == File::FS_OFFERED) {
        const auto preview = DsEngine::instance().getFileManager()->thumbnailFor(*file_);
        if (preview && preview->getState() == File::FS_PAUSED &&
            DsEngine::instance().transfers().find(preview->checkpointKey().value())) {
            // Preview recovery repeats the complete signed manifest, including
            // this undecided primary. It needs no second offer preparation.
            co_return Outcome{Outcome::Kind::Offered, dsfm::TransferPauseReason::Disconnected};
        }
    }
    setPhase(Phase::Negotiating);
    if (start_ != OutgoingTransferStart::Resume) {
        auto offered = co_await offer();
        if (!offered) co_return sourceError(offered.error(), offered.error().code == dsfm::ErrorCode::IntegrityError);
        if (start_ != OutgoingTransferStart::ManifestMember && !request_ && !pendingDecision())
            setDeadline(QDeadlineTimer{QDeadlineTimer::Forever});
        if (reader_) {
            auto closed = co_await reader_->close(); reader_.reset(); verifier_.reset();
            if (!closed) co_return sourceError(closed.error());
        }
        parkAdmission();
    }
    auto negotiated = co_await negotiate();
    if (!negotiated) {
        if (negotiated.error().code == dsfm::ErrorCode::InvalidArgument) peer_->close();
        co_return sourceError(negotiated.error(), negotiated.error().code == dsfm::ErrorCode::IntegrityError);
    }
    if (!current() || !selected_) co_return interrupted();
    last_ = selected_->selected;
    offset_ = last_.offset;
    file_->setState(File::FS_TRANSFERRING);
    file_->setChannel(peer_->bindTransfer(*file_, DsEngine::instance().transfers().find(key())));
    setPhase(Phase::Transferring);
    setDeadline(QDeadlineTimer{QDeadlineTimer::Forever});
    dsfm::CheckpointWindow window{quint64(file_->getSize()), selected_->interval, offset_};
    quint64 checkpoint_sequence = file_->getSize() == 0 ? 0 : selected_->next_sequence;
    while (current()) {
        if (in_flight_.size() >= 8 || !peer_->transferWritable()) {
            setDeadline(QDeadlineTimer{30000});
            while (current() && (in_flight_.size() >= 8 || !peer_->transferWritable())) {
                auto event = co_await nextEvent();
                if (!event || !current()) break;
                if (redundant(*event)) continue;
                if (!acknowledge(*event)) { peer_->close(); stop(Stop::Disconnected); break; }
            }
            setDeadline(QDeadlineTimer{QDeadlineTimer::Forever});
            continue;
        }
        const auto count = qsizetype(std::min<quint64>({8192, window.allowance(), quint64(file_->getSize()) - offset_}));
        auto bytes = co_await reader_->readAt(offset_, count, cancellation());
        if (!current()) break;
        if (!bytes) co_return sourceError(bytes.error());
        if (bytes.value().size() != count) co_return sourceError(dsfm::Error{dsfm::ErrorCode::ExternalModification});
        const bool final = offset_ + quint64(count) == quint64(file_->getSize());
        auto checked = co_await hashBytes(offset_, bytes.value());
        if (checked && final) checked = co_await verifier_->finish(cancellation());
        if (!checked) {
            if (checked.error().code == dsfm::ErrorCode::IntegrityError) co_await reader_->invalidate();
            co_return sourceError(checked.error(), true);
        }
        if (!current()) break;
        if (!window.admit(offset_, quint64(count))) throw std::logic_error("Checkpoint payload fence violated");
        const auto sequence = next_sequence_++;
        const auto offset = offset_;
        offset_ += quint64(count);
        in_flight_.emplace(next_sequence_, offset_);
        peer_->sendCheckpointChunk(file_->getFileId(), attempt(), sequence, offset, bytes.value(), final);
        if (final || !window.allowance()) {
            auto prefix = co_await verifier_->prefix(cancellation());
            if (!prefix || !current()) break;
            const dsfm::DurableCheckpoint checkpoint{checkpoint_sequence++, offset_, prefix.value().sha256, final, false};
            auto confirmed = co_await confirm(checkpoint);
            if (!confirmed) {
                if (confirmed.error().code == dsfm::ErrorCode::IntegrityError)
                    co_return Outcome{Outcome::Kind::Failed, dsfm::TransferPauseReason::None, dsfm::TransferFailureReason::ReceiverCheckpointMismatch};
                if (!stopped() && confirmed.error().code == dsfm::ErrorCode::InvalidArgument) peer_->close();
                co_return sourceError(confirmed.error());
            }
            if (!window.confirm(offset_)) throw std::logic_error("Checkpoint confirmation fence violated");
            setDeadline(QDeadlineTimer{QDeadlineTimer::Forever});
        }
        if (final) {
            setDeadline(QDeadlineTimer{30000});
            while (current() && !final_ack_) {
                auto event = co_await nextEvent();
                if (!event || !current()) break;
                if (redundant(*event)) continue;
                if (!acknowledge(*event)) { peer_->close(); break; }
            }
            if (!current() || !final_ack_) break;
            auto complete = co_await storage.transferProgress().complete(key(), attempt(), true, true);
            if (!complete) co_return sourceError(complete.error());
            co_return Outcome{Outcome::Kind::Completed};
        }
    }
    if (!stopped()) stop(Stop::Disconnected);
    co_return interrupted();
}

QCoro::Task<TransferSession::Outcome> OutgoingTransfer::finalize(Outcome result) {
    auto release = qScopeGuard([&] {
        if (peer_) peer_->releaseTransfer(*file_, false, {}, 0, 0);
        operation_.reset();
    });
    if (reader_) {
        auto closed = co_await reader_->close(); reader_.reset();
        if (!closed && result.kind != Outcome::Kind::Completed) result = sourceError(closed.error());
    }
    verifier_.reset();
    if (result.kind == Outcome::Kind::Completed) { file_->transferComplete(); co_return result; }
    auto &storage = DsEngine::instance().getFileManager()->storage();
    auto ready = co_await storage.ready();
    if (!ready) throw std::runtime_error("Transfer journal unavailable during finalization");
    auto saved = co_await storage.transferProgress().create({key(), quint64(file_->getSize()), file_->getHash(),
        file_->checkpointInterval() ? file_->checkpointInterval() : dsfm::default_checkpoint_interval, dsfm::TransferSide::Sender});
    if (!saved) throw std::runtime_error("Transfer journal identity cannot be settled");
    if (saved.value().complete) {
        file_->projectTransferOutcome(saved.value().description.interval, dsfm::TransferPauseReason::None, dsfm::TransferFailureReason::None, File::FS_DONE);
        co_return Outcome{Outcome::Kind::Completed};
    }
    if (saved.value().failure != dsfm::TransferFailureReason::None && saved.value().failure != result.failure)
        result = {Outcome::Kind::Failed, dsfm::TransferPauseReason::None, saved.value().failure};
    const bool terminal = result.kind == Outcome::Kind::Failed || result.kind == Outcome::Kind::Cancelled || result.kind == Outcome::Kind::Rejected;
    dsfm::Result<void> recorded;
    if (terminal && saved.value().failure == dsfm::TransferFailureReason::None)
        recorded = co_await storage.transferProgress().fail(key(), result.failure, saved.value().attempt);
    else if (!terminal) recorded = co_await storage.transferProgress().pause(key(), saved.value().attempt, result.pause);
    if (!recorded) throw std::runtime_error("Transfer outcome could not be persisted");
    if (result.kind != Outcome::Kind::Offered)
        file_->projectTransferOutcome(saved.value().description.interval, terminal ? dsfm::TransferPauseReason::None : result.pause,
            terminal ? result.failure : dsfm::TransferFailureReason::None,
            result.kind == Outcome::Kind::Cancelled ? File::FS_CANCELLED : result.kind == Outcome::Kind::Rejected ? File::FS_REJECTED :
            result.kind == Outcome::Kind::Failed ? File::FS_FAILED : File::FS_PAUSED);
    if (peer_ && peer_->isConnected()) {
        try {
            if (terminal && result.kind != Outcome::Kind::Rejected && result.failure != dsfm::TransferFailureReason::UnsupportedVersion) {
                const auto reason = result.failure == dsfm::TransferFailureReason::SourceIntegrityMismatch ? FileCancelReason::SourceChanged :
                    result.failure == dsfm::TransferFailureReason::ReceiverCheckpointMismatch ? FileCancelReason::ReceiverCheckpointMismatch : FileCancelReason::UserAction;
                if (resume_request_) peer_->sendCheckpointResumeResult(*resume_request_, file_->getFileId(), FileResumeStatus::Cancelled, {}, dsfm::TransferPauseReason::None, reason);
                else peer_->sendFileCancel(file_->getFileId(), reason);
            } else if (!terminal && result.kind != Outcome::Kind::Offered) {
                if (resume_request_) peer_->sendCheckpointResumeResult(*resume_request_, file_->getFileId(), FileResumeStatus::Paused, {}, result.pause);
                else if (selected_ && file_->getChannel()) peer_->sendFilePause(file_->getFileId(), attempt(), last_, result.pause);
            }
        } catch (...) {}
    }
    co_return result;
}
} // namespace ds::core
