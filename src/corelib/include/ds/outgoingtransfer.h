#pragma once
#include "ds/transfersession.h"
#include "ds/file.h"
#include "ds/peerconnection.h"
#include "ds/managedstorage.h"
#include <dsfilemanager/transferverifier.h>
#include <map>

namespace ds::core {
enum class OutgoingTransferStart { Offer, Resume, Recover, ManifestMember };
class OutgoingTransfer final : public TransferSession {
public:
    OutgoingTransfer(File::ptr_t file, PeerConnection::ptr_t peer, OutgoingTransferStart start,
                     std::optional<Event> request = {});
    ~OutgoingTransfer() override;
protected:
    QCoro::Task<Outcome> execute() override;
    QCoro::Task<Outcome> finalize(Outcome result) override;
private:
    bool current();
    QCoro::Task<dsfm::Result<void>> openSource();
    QCoro::Task<dsfm::Result<void>> hashBytes(quint64 offset, QByteArray bytes);
    QCoro::Task<dsfm::Result<void>> verifyPrefix(dsfm::DurableCheckpoint selected);
    QCoro::Task<dsfm::Result<void>> offer();
    QCoro::Task<dsfm::Result<void>> negotiate();
    QCoro::Task<dsfm::Result<void>> confirm(dsfm::DurableCheckpoint checkpoint);
    bool acknowledge(const Event &event);
    bool redundant(const Event &event);
    Outcome sourceError(dsfm::Error error, bool proof = false);
    File::ptr_t file_, preview_;
    PeerConnection::ptr_t peer_;
    OutgoingTransferStart start_;
    std::optional<Event> request_;
    std::optional<quint64> resume_request_;
    std::optional<ManagedStorage::Operation> operation_;
    std::unique_ptr<dsfm::IFileReader> reader_;
    std::unique_ptr<dsfm::TransferVerifier> verifier_;
    std::optional<dsfm::TransferAttempt> selected_;
    QVector<dsfm::DurableCheckpoint> trusted_;
    dsfm::DurableCheckpoint last_;
    std::vector<QMetaObject::Connection> observers_;
    std::map<quint64, quint64> in_flight_;
    quint64 next_sequence_ = 0, offset_ = 0, ack_sequence_ = 0, ack_offset_ = 0;
    bool final_ack_ = false;
};
} // namespace ds::core
