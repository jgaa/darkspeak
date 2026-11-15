#pragma once
#include "internal.h"
#include <QAbstractEventDispatcher>
#include <QThread>
#include <logfault/logfault.h>

namespace dsfm::detail {
struct Job { QUuid key; quint64 bytes; std::function<void()> run; QUuid group{}; std::function<void()> completed{}; };
struct Scheduler : std::enable_shared_from_this<Scheduler> {
    std::mutex mutex;
    std::condition_variable ready;
    std::map<QUuid, std::deque<Job>> queues;
    QSet<QUuid> running;
    QHash<QUuid, quint32> group_counts;
    QHash<QUuid, quint64> group_bytes;
    QUuid last;
    quint64 bytes = 0;
    quint32 count = 0;
    bool stopping = false;
    static std::shared_ptr<Scheduler> create() {
        auto state = std::make_shared<Scheduler>();
        for (int i = 0; i < 4; ++i) std::thread([state] { state->loop(); }).detach();
        return state;
    }
    bool enqueue(Job job, quint32 max_count, quint64 max_bytes, bool control = false) {
        std::lock_guard lock(mutex);
        if (job.group.isNull()) job.group = job.key;
        if (stopping || count >= (control ? 132096U : 256U) || (!control && group_counts.value(job.group) >= max_count)
            || job.bytes > max_bytes || job.bytes > (64ULL << 20) || bytes > (64ULL << 20) - job.bytes
            || group_bytes.value(job.group) > max_bytes - job.bytes) return false;
        ++count; bytes += job.bytes; ++group_counts[job.group]; group_bytes[job.group] += job.bytes;
        queues[job.key].push_back(std::move(job)); ready.notify_all(); return true;
    }
    bool reserveBytes(QUuid group, quint64 amount, quint64 maximum) {
        std::lock_guard lock(mutex);
        if (amount > maximum || amount > (64ULL << 20) || bytes > (64ULL << 20) - amount
            || group_bytes.value(group) > maximum - amount) return false;
        bytes += amount; group_bytes[group] += amount; return true;
    }
    void releaseBytes(QUuid group, quint64 amount) {
        std::lock_guard lock(mutex); bytes -= amount;
        group_bytes[group] -= amount; if (!group_bytes[group]) group_bytes.remove(group);
    }
    void loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { if (stopping && !count) return true; for (auto& [key, queue] : queues) if (!queue.empty() && !running.contains(key)) return true; return false; });
                if (stopping && !count) return;
                auto it = queues.upper_bound(last);
                for (size_t attempts = 0; attempts < queues.size(); ++attempts) {
                    if (it == queues.end()) it = queues.begin();
                    if (!it->second.empty() && !running.contains(it->first)) break;
                    ++it;
                }
                last = it->first; running.insert(last); job = std::move(it->second.front()); it->second.pop_front();
            }
            try { job.run(); } catch (...) { LFLOG_ERROR << "FileManager cleanup/background job failed"; }
            {
                std::lock_guard lock(mutex);
                running.remove(job.key); --count; bytes -= job.bytes;
                if (--group_counts[job.group] == 0) group_counts.remove(job.group);
                group_bytes[job.group] -= job.bytes; if (!group_bytes[job.group]) group_bytes.remove(job.group);
                auto it = queues.find(job.key); if (it != queues.end() && it->second.empty()) queues.erase(it);
            }
            ready.notify_all();
            if (job.completed) job.completed();
        }
    }
    void stop() { std::lock_guard lock(mutex); stopping = true; ready.notify_all(); }
};
struct BufferReservation {
    std::shared_ptr<Scheduler> scheduler; QUuid group; quint64 bytes;
    ~BufferReservation() { if (scheduler) scheduler->releaseBytes(group, bytes); }
};
template<typename T> struct AsyncState { std::optional<Result<T>> result; std::shared_ptr<BufferReservation> reservation; };
template<typename T> struct WorkAwaiter {
    std::shared_ptr<Scheduler> scheduler;
    QUuid key;
    quint64 bytes;
    quint32 max_count;
    quint64 max_bytes;
    std::function<Result<T>()> function;
    QUuid group;
    std::shared_ptr<AsyncState<T>> state = std::make_shared<AsyncState<T>>();
    bool await_ready() const { return false; }
    bool await_suspend(std::coroutine_handle<> continuation) {
        auto* dispatcher = QAbstractEventDispatcher::instance(QThread::currentThread());
        if (!dispatcher) { state->result.emplace(Error{ErrorCode::InvalidArgument}); return false; }
        const auto budget_group = group.isNull() ? key : group;
        if (!scheduler->reserveBytes(budget_group, bytes, max_bytes)) { state->result.emplace(Error{ErrorCode::QueueFull}); return false; }
        state->reservation = std::make_shared<BufferReservation>();
        state->reservation->scheduler = scheduler; state->reservation->group = budget_group; state->reservation->bytes = bytes;
        auto result = state;
        QPointer<QObject> context(dispatcher);
        auto run = [result, fn = std::move(function)]() mutable {
            try { result->result.emplace(fn()); }
            catch (const Failure& failure) {
                LFLOG_DEBUG << "FileManager operation failed: code=" << int(failure.error.code) << ", native=" << failure.error.native_code;
                result->result.emplace(failure.error);
            } catch (const std::bad_alloc&) { result->result.emplace(Error{ErrorCode::QuotaExceeded}); }
            catch (...) { result->result.emplace(Error{ErrorCode::IoError}); }
        };
        auto completed = [result, context, continuation] {
            if (context) QMetaObject::invokeMethod(context, [result, continuation] { continuation.resume(); }, Qt::QueuedConnection);
        };
        if (!scheduler->enqueue({key, 0, std::move(run), group, std::move(completed)}, max_count, max_bytes, max_count == 4096)) {
            state->result.emplace(Error{ErrorCode::QueueFull}); return false;
        }
        return true;
    }
    Result<T> await_resume() { state->reservation.reset(); return std::move(state->result.value()); }
};
template<typename T> QCoro::Task<Result<T>> execute(std::shared_ptr<Scheduler> scheduler, QUuid key, quint64 bytes,
        quint32 count, quint64 max_bytes, std::function<Result<T>()> function, QUuid group = {}) {
    // Named awaiter keeps nontrivial captures out of a temporary coroutine expression.
    WorkAwaiter<T> work{std::move(scheduler), key, bytes, count, max_bytes, std::move(function), group};
    co_return co_await work;
}
} // namespace dsfm::detail
