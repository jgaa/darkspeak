#include <dsfilemanager/transferverifier.h>
#include "async_p.h"
#include <QCryptographicHash>
namespace dsfm {
using namespace detail;
struct TransferVerifier::Impl {
    std::shared_ptr<Scheduler> scheduler;
    QUuid lane = QUuid::createUuid();
    quint64 size = 0, offset = 0;
    QByteArray expected;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    std::atomic_bool failed{false};
    bool complete = false;
    Impl() {
        // One bounded pool shared by all active verifiers.
        static std::mutex mutex; static std::weak_ptr<Scheduler> weak;
        std::lock_guard lock(mutex); scheduler = weak.lock();
        if (!scheduler) { scheduler = Scheduler::create(); weak = scheduler; }
    }
};
TransferVerifier::TransferVerifier(quint64 expected_size, QByteArray expected_sha256) : impl_(std::make_shared<Impl>()) {
    impl_->size = expected_size;
    if (expected_sha256.size() == 32 && expected_size <= quint64(std::numeric_limits<qint64>::max())) impl_->expected = std::move(expected_sha256);
    else impl_->failed = true;
}
TransferVerifier::~TransferVerifier() = default;
QCoro::Task<Result<void>> TransferVerifier::add(quint64 offset, QByteArray bytes, CancellationToken cancel) {
    auto state = impl_;
    if (bytes.size() > (1 << 20)) { state->failed = true; co_return Error{ErrorCode::InvalidArgument}; }
    const auto byte_count = quint64(bytes.size());
    std::function<Result<void>()> work = [state, offset, bytes = std::move(bytes), cancel]() -> Result<void> {
        if (cancel.cancelled()) { state->failed = true; return Error{ErrorCode::Cancelled}; }
        if (state->failed || state->complete || state->expected.size() != 32 || offset != state->offset
            || offset > state->size || quint64(bytes.size()) > state->size - offset) {
            state->failed = true; return Error{ErrorCode::IntegrityError};
        }
        state->hash.addData(bytes); state->offset += quint64(bytes.size()); return {};
    };
    auto result = co_await execute<void>(state->scheduler, state->lane, byte_count, 16, 4U << 20, std::move(work));
    if (!result) state->failed = true;
    co_return result;
}
QCoro::Task<Result<void>> TransferVerifier::finish(CancellationToken cancel) {
    auto state = impl_;
    std::function<Result<void>()> work = [state, cancel]() -> Result<void> {
        if (cancel.cancelled()) { state->failed = true; return Error{ErrorCode::Cancelled}; }
        if (state->failed || state->complete || state->expected.size() != 32 || state->offset != state->size || state->hash.result() != state->expected) {
            state->failed = true; return Error{ErrorCode::IntegrityError};
        }
        state->complete = true; return {};
    };
    auto result = co_await execute<void>(state->scheduler, state->lane, 0, 16, 4U << 20, std::move(work));
    if (!result) state->failed = true;
    co_return result;
}
} // namespace dsfm
