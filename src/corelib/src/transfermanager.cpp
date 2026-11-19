#include "ds/transfermanager.h"

#include <QDebug>
#include <QCoroSignal>
#include <QCoroTimer>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <vector>
#include <set>
#include <stdexcept>

namespace ds::core {

struct TransferReservation::Budget {
    std::set<std::tuple<QUuid, QUuid, QUuid, dsfm::TransferSide>> occupied;
    QPointer<TransferManager> manager;
    bool closing = false;
};
struct TransferManager::Registry {
    std::map<Key, Entry> sessions;
    std::deque<Key> queued;
};

TransferManager::Key TransferManager::index(const dsfm::TransferKey &key) {
    return {key.local_scope, key.remote_scope, key.transfer, key.side};
}
TransferReservation::TransferReservation(std::shared_ptr<Budget> budget, dsfm::TransferKey key)
    : budget_{std::move(budget)}, key_{key} {}
TransferReservation::~TransferReservation() {
    budget_->occupied.erase({key_.local_scope, key_.remote_scope, key_.transfer, key_.side});
    if (budget_->manager) {
        const auto manager = budget_->manager;
        QMetaObject::invokeMethod(manager, [manager] { if (manager) manager->schedule(); }, Qt::QueuedConnection);
    }
}

TransferManager::TransferManager(QObject *parent)
    : QObject{parent}, budget_{std::make_shared<TransferReservation::Budget>()}, registry_{std::make_shared<Registry>()} {
    budget_->manager = this;
}
TransferManager::~TransferManager() {
    budget_->closing = true;
    budget_->manager = nullptr;
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        entry.session->stop(TransferSession::Stop::Disconnected);
    }
    // Running roots retain the registry until their awaited I/O/finalization
    // returns. Normal shutdown awaits them before destroying application services.
}

size_t TransferManager::active(QUuid local_scope, QUuid remote_scope) const {
    size_t count = 0;
    for (const auto &key : budget_->occupied)
        if (std::get<0>(key) == local_scope && std::get<1>(key) == remote_scope) ++count;
    return count;
}

dsfm::Result<std::shared_ptr<TransferReservation>> TransferManager::reserve(const dsfm::TransferKey &key) {
    if (budget_->closing) return dsfm::Error{dsfm::ErrorCode::ShuttingDown};
    return reserveActive(key);
}

dsfm::Result<std::shared_ptr<TransferReservation>> TransferManager::reserveActive(const dsfm::TransferKey &key) {
    if (QThread::currentThread() != thread() || key.local_scope.isNull() || key.remote_scope.isNull() || key.transfer.isNull())
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    const auto id = index(key);
    if (budget_->occupied.contains(id) || active(key.local_scope, key.remote_scope) >= 2)
        return dsfm::Error{dsfm::ErrorCode::Busy};
    budget_->occupied.insert(id);
    return std::shared_ptr<TransferReservation>{new TransferReservation{budget_, key}};
}

dsfm::Result<void> TransferManager::submit(std::shared_ptr<TransferSession> session) {
    if (!session || QThread::currentThread() != thread() || session->thread() != thread() ||
        session->parent() || session->manager_ || session->phase() != TransferSession::Phase::Preparing ||
        (session->connection().isNull() && session->accepting()))
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    if (budget_->closing) return dsfm::Error{dsfm::ErrorCode::ShuttingDown};
    const auto &key = session->key();
    if (key.local_scope.isNull() || key.remote_scope.isNull() || key.transfer.isNull())
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    const auto id = index(key);
    if (registry_->sessions.contains(id)) return dsfm::Error{dsfm::ErrorCode::Busy};
    size_t registered = 0;
    for (const auto &[pending, entry] : registry_->sessions) {
        Q_UNUSED(entry);
        if (std::get<0>(pending) == key.local_scope && std::get<1>(pending) == key.remote_scope) ++registered;
    }
    if (registered >= 64 + 2) return dsfm::Error{dsfm::ErrorCode::QueueFull};
    session->manager_ = this;
    registry_->sessions.emplace(id, Entry{std::move(session), {}, {}});
    registry_->queued.push_back(id);
    schedule();
    return {};
}

std::shared_ptr<TransferSession> TransferManager::find(const dsfm::TransferKey &key) const {
    const auto it = registry_->sessions.find(index(key));
    return it == registry_->sessions.end() ? nullptr : it->second.session;
}

std::shared_ptr<TransferSession> TransferManager::findLocalFile(int local_file) const {
    if (local_file <= 0) return {};
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        if (entry.session->localFile() == local_file) return entry.session;
    }
    return {};
}

void TransferManager::schedule() {
    // Called after admissions/releases, never by a periodic timer.
    std::vector<Key> ready;
    for (auto it = registry_->queued.begin(); it != registry_->queued.end();) {
        const auto id = *it;
        auto &entry = registry_->sessions.at(id);
        auto lease = reserveActive(entry.session->key());
        if (!lease) { ++it; continue; }
        entry.reservation = std::move(lease).value();
        it = registry_->queued.erase(it);
        ready.push_back(id);
    }
    for (const auto &id : ready) {
        auto &entry = registry_->sessions.at(id);
        if (!entry.root) entry.root.emplace(run(registry_, id, this));
    }
    emit admissionChanged();
}

void TransferManager::park(const dsfm::TransferKey &key) {
    registry_->sessions.at(index(key)).reservation.reset();
}

QCoro::Task<void> TransferManager::acquire(const dsfm::TransferKey &key) {
    const auto id = index(key);
    if (registry_->sessions.at(id).reservation) co_return;
    if (std::find(registry_->queued.begin(), registry_->queued.end(), id) == registry_->queued.end())
        registry_->queued.push_back(id);
    schedule();
    while (!registry_->sessions.at(id).reservation) co_await qCoro(this, &TransferManager::admissionChanged);
}

QCoro::Task<void> TransferManager::run(std::shared_ptr<Registry> registry, Key key, QPointer<TransferManager> manager) {
    auto session = registry->sessions.at(key).session;
    // QCoro tasks start eagerly. Queue execution so the retained root is in
    // the registry before model notifications can reenter application shutdown.
    QTimer launch;
    launch.setSingleShot(true);
    launch.start(0);
    co_await qCoro(launch).waitForTimeout();
    co_await session->run();
    registry->sessions.at(key).reservation.reset();
    // Reap the retained root after it returns, not from its own stack.
    QTimer::singleShot(0, [registry, key, transfer_key = session->key(), manager] {
        registry->sessions.erase(key);
        if (manager) emit manager->attemptFinished(transfer_key);
    });
}

void TransferManager::retireConnection(QUuid connection) {
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        if (entry.session->connection() == connection) entry.session->stop(TransferSession::Stop::Disconnected);
    }
}

QCoro::Task<void> TransferManager::shutdown() {
    budget_->closing = true;
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        entry.session->stop(TransferSession::Stop::Disconnected);
    }
    // Keep each root alive while awaiting it; queued reaping cannot invalidate it.
    while (!registry_->sessions.empty()) {
        schedule();
        auto it = std::find_if(registry_->sessions.begin(), registry_->sessions.end(), [](const auto &entry) {
            return entry.second.root.has_value();
        });
        if (it == registry_->sessions.end()) {
            co_await qCoro(this, &TransferManager::admissionChanged);
            continue;
        }
        const auto key = it->first;
        auto root = std::move(*it->second.root);
        co_await root;
        registry_->sessions.erase(key);
    }
}

} // namespace ds::core
