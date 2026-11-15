#pragma once
#include <dsfilemanager/filemanager.h>
#include <QThreadPool>
#include <QAbstractEventDispatcher>
#include <QThread>
#include <coroutine>

namespace dsvfs::detail {
struct Worker {
    QThreadPool pool;
    std::atomic_uint count{0};
    Worker() { pool.setMaxThreadCount(2); }
};
inline Worker &worker() {
    static Worker value;
    return value;
}
template <class T> struct Work {
    struct State {
        std::function<dsfm::Result<T>()> fn;
        std::optional<dsfm::Result<T>> result;
    };
    std::shared_ptr<State> state;
    bool await_ready() const { return false; }
    bool await_suspend(std::coroutine_handle<> continuation) {
        auto *dispatcher = QAbstractEventDispatcher::instance(QThread::currentThread());
        if (!dispatcher) {
            state->result.emplace(dsfm::Error{dsfm::ErrorCode::InvalidArgument});
            return false;
        }
        auto &owner = worker();
        if (owner.count.fetch_add(1) >= 32) {
            --owner.count;
            state->result.emplace(dsfm::Error{dsfm::ErrorCode::QueueFull});
            return false;
        }
        QPointer<QObject> context(dispatcher);
        owner.pool.start([saved = state, context, continuation] {
            try {
                saved->result.emplace(saved->fn());
            } catch (const std::bad_alloc &) {
                saved->result.emplace(dsfm::Error{dsfm::ErrorCode::QuotaExceeded});
            } catch (...) {
                saved->result.emplace(dsfm::Error{dsfm::ErrorCode::IoError});
            }
            --worker().count;
            if (context)
                QMetaObject::invokeMethod(
                    context, [saved, continuation] { continuation.resume(); }, Qt::QueuedConnection);
        });
        return true;
    }
    dsfm::Result<T> await_resume() { return std::move(*state->result); }
};
template <class T, class F> QCoro::Task<dsfm::Result<T>> work(F fn) {
    auto state = std::make_shared<typename Work<T>::State>();
    state->fn = std::move(fn);
    Work<T> operation{std::move(state)};
    co_return co_await operation;
}
} // namespace dsvfs::detail
