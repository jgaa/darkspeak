#pragma once
#include <dsfilemanager/filemanager.h>

namespace dsfm {
inline constexpr quint32 checkpoint_format_version = 1;
inline constexpr quint32 minimum_checkpoint_interval = 64U << 10;
inline constexpr quint32 default_checkpoint_interval = 1U << 20;
inline constexpr quint32 maximum_checkpoint_interval = 16U << 20;
inline constexpr quint32 maximum_retained_checkpoints = 64;
bool validCheckpointInterval(quint32 interval) noexcept;
Result<quint32> negotiateCheckpointInterval(quint32 local, quint32 remote);
struct TransferKey {
    QUuid local_scope, remote_scope, transfer;
    bool operator==(const TransferKey &) const = default;
};
enum class TransferSide { Sender, Receiver };
struct TransferDescription {
    TransferKey key;
    quint64 size = 0;
    QByteArray sha256;
    quint32 interval = default_checkpoint_interval;
    TransferSide side = TransferSide::Sender;
};
struct DurableCheckpoint {
    quint64 sequence = 0, offset = 0;
    QByteArray sha256;
    bool final = false;
    bool pause_boundary = false;
    bool operator==(const DurableCheckpoint &) const = default;
};
struct TransferAttempt {
    QUuid id;
    quint32 interval = default_checkpoint_interval;
    quint64 next_sequence = 1;
    DurableCheckpoint selected;
};
enum class CheckpointPhase { Proposed, SenderConfirmed, MutuallyConfirmed };
enum class TransferPauseReason {
    None,
    Disconnected,
    SourceUnavailable,
    SourceRevalidationRequired,
    ResourceUnavailable,
    AuthorizationWithdrawn,
    FeaturePaused
};
enum class TransferFailureReason {
    None,
    SourceIntegrityMismatch,
    ReceiverCheckpointMismatch,
    UnsupportedVersion,
    UserAction
};
struct DurableTransferProgress {
    TransferDescription description;
    QUuid attempt;
    quint64 confirmed_offset = 0;
    quint64 next_checkpoint_sequence = 1;
    QVector<DurableCheckpoint> checkpoints; // Mutually confirmed, including zero.
    QVector<DurableCheckpoint> accepted_checkpoints; // Sender-persisted confirmations.
    QVector<DurableCheckpoint> durable_checkpoints; // Includes receiver proposals for recovery negotiation.
    bool complete = false;
    TransferPauseReason pause = TransferPauseReason::None;
    TransferFailureReason failure = TransferFailureReason::None;
};
// Admission fence independent of flow-control ACKs and feature accounting.
// The transport owns its separate eight-chunk in-flight window.
class CheckpointWindow {
  public:
    CheckpointWindow(quint64 size, quint32 interval, quint64 confirmed = 0);
    quint64 allowance() const noexcept;
    Result<void> admit(quint64 offset, quint64 length);
    Result<void> confirm(quint64 offset, bool pause_boundary = false);
    quint64 produced() const noexcept { return produced_; }
    quint64 confirmed() const noexcept { return confirmed_; }

  private:
    quint64 size_ = 0, produced_ = 0, confirmed_ = 0;
    quint32 interval_ = 0;
    bool valid_ = false;
};
// Generic durable progress metadata. It contains no VFS, credits, feature
// purposes, payload storage, or serialized SHA implementation state. Callers
// supply authenticated opaque scopes and locally verified prefix digests.
class TransferProgressStore {
  public:
    struct Impl;
    explicit TransferProgressStore(QString private_root, quint32 max_transfers = 4096,
                                   std::shared_ptr<StoragePool> storage_pool = {});
    ~TransferProgressStore();
    QCoro::Task<Result<void>> open();
    QCoro::Task<Result<DurableTransferProgress>> create(TransferDescription description);
    QCoro::Task<Result<DurableTransferProgress>> load(TransferKey key);
    QCoro::Task<Result<DurableCheckpoint>> selectResume(TransferKey key, QVector<DurableCheckpoint> retained);
    QCoro::Task<Result<void>> beginAttempt(TransferKey key, QUuid attempt, DurableCheckpoint selected, quint64 next_sequence = 0);
    QCoro::Task<Result<void>> record(TransferKey key, QUuid attempt, DurableCheckpoint checkpoint,
                                     CheckpointPhase phase);
    QCoro::Task<Result<void>> complete(TransferKey key, QUuid attempt, bool full_hash_verified, bool final_flow_ack);
    QCoro::Task<Result<void>> pause(TransferKey key, QUuid attempt, TransferPauseReason reason);
    QCoro::Task<Result<void>> fail(TransferKey key, TransferFailureReason reason);
    QCoro::Task<Result<void>> shutdown();

  private:
    std::shared_ptr<Impl> impl_;
};
} // namespace dsfm

Q_DECLARE_METATYPE(dsfm::DurableCheckpoint)
