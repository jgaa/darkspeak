#pragma once

#include "ds/transfersession.h"
#include "ds/file.h"
#include "ds/peerconnection.h"
#include "ds/managedstorage.h"

namespace ds::core {

enum class IncomingTransferStart { Accept, Resume, Completed, PublishedRecovery };

class IncomingTransfer final : public TransferSession {
public:
    using Start = IncomingTransferStart;
    IncomingTransfer(File::ptr_t file, PeerConnection::ptr_t peer, Start start,
                     QDeadlineTimer deadline = QDeadlineTimer{QDeadlineTimer::Forever});
    ~IncomingTransfer() override;
protected:
    QCoro::Task<Outcome> execute() override;
    QCoro::Task<Outcome> finalize(Outcome outcome) override;
private:
    bool authorized() const;
    bool current() const;
    QCoro::Task<bool> selectCheckpoint();
    QCoro::Task<bool> confirm(dsfm::DurableCheckpoint checkpoint);
    File::ptr_t file_;
    PeerConnection::ptr_t peer_;
    const Start start_;
    QDeadlineTimer negotiation_deadline_;
    File::ptr_t primary_;
    std::optional<ManagedStorage::Operation> operation_;
    std::unique_ptr<dsfm::IFileWriter> writer_;
    std::optional<dsfm::TransferAttempt> selected_;
    dsfm::DurableCheckpoint last_;
    std::vector<QMetaObject::Connection> observers_;
    quint64 sequence_ = 0, offset_ = 0, final_start_ = 0;
    bool completed_content_ = false;
};

} // namespace ds::core
