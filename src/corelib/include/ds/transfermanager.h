#pragma once

#include "ds/transfersession.h"
#include <QPointer>
#include <map>
#include <tuple>

namespace ds::core {

// Admission lease for sessions and bounded startup journal reconciliation.
// It remains occupied until the owner has joined and settled its work.
class TransferReservation {
public:
    ~TransferReservation();
private:
    friend class TransferManager;
    struct Budget;
    TransferReservation(std::shared_ptr<Budget> budget, dsfm::TransferKey key);
    std::shared_ptr<Budget> budget_;
    dsfm::TransferKey key_;
};

class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject *parent = nullptr);
    ~TransferManager() override;
    dsfm::Result<std::shared_ptr<TransferReservation>> reserve(const dsfm::TransferKey &key);
    size_t active(QUuid local_scope, QUuid remote_scope) const;
    dsfm::Result<void> submit(std::shared_ptr<TransferSession> session);
    std::shared_ptr<TransferSession> find(const dsfm::TransferKey &key) const;
    std::shared_ptr<TransferSession> findLocalFile(int local_file) const;
    QCoro::Task<void> shutdown();
    void retireConnection(QUuid connection);

signals:
    void admissionChanged();
    void attemptFinished(dsfm::TransferKey key);

private:
    friend class TransferReservation;
    friend class TransferSession;
    using Key = std::tuple<QUuid, QUuid, QUuid, dsfm::TransferSide>;
    static Key index(const dsfm::TransferKey &key);
    struct Entry {
        std::shared_ptr<TransferSession> session;
        std::shared_ptr<TransferReservation> reservation;
        std::optional<QCoro::Task<void>> root;
    };
    struct Registry;
    static QCoro::Task<void> run(std::shared_ptr<Registry> registry, Key key, QPointer<TransferManager> manager);
    dsfm::Result<std::shared_ptr<TransferReservation>> reserveActive(const dsfm::TransferKey &key);
    void schedule();
    void park(const dsfm::TransferKey &key);
    QCoro::Task<void> acquire(const dsfm::TransferKey &key);
    std::shared_ptr<TransferReservation::Budget> budget_;
    std::shared_ptr<Registry> registry_;
};

} // namespace ds::core
