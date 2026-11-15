#pragma once
#include <dsfilemanager/filemanager.h>

namespace dsfm {
// Hash logical-order payload bytes, including a rebuilt prefix on suffix resume.
// Duplicate/retransmitted chunks must not be added a second time. Any failure is
// terminal for this attempt; finish requires the exact full expected byte count.
struct TransferPrefix { quint64 size = 0; QByteArray sha256; };
class TransferVerifier {
public:
    struct Impl;
    TransferVerifier(quint64 expected_size, QByteArray expected_sha256);
    ~TransferVerifier();
    TransferVerifier(const TransferVerifier&) = delete;
    TransferVerifier& operator=(const TransferVerifier&) = delete;
    QCoro::Task<Result<void>> add(quint64 offset, QByteArray bytes, CancellationToken cancel = {});
    QCoro::Task<Result<TransferPrefix>> prefix(CancellationToken cancel = {});
    QCoro::Task<Result<void>> finish(CancellationToken cancel = {});
private:
    std::shared_ptr<Impl> impl_;
};
} // namespace dsfm
