#pragma once
#include "policy.h"
#include "store.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace ds::core {
class DsEngine;
}
namespace ds::mcp {
struct Tool {
    QString name, capability, permission;
    QStringList fields, required;
    bool gated = false, mutates = false;
};
const QList<Tool>& tools();
struct Reply {
    std::mutex mutex;
    bool ready = false;
    QJsonObject value;
    QString error;
    void finish(QJsonObject result, QString code = {});
};
class Transport;
#ifdef DARKSPEAK_WITH_TEST_MCP
class TestSupport;
#endif
class Service final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QVariantList pending READ pending NOTIFY changed)
    Q_PROPERTY(QVariantList activity READ activity NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    Q_PROPERTY(QVariantList keys READ keys NOTIFY changed)
    Q_PROPERTY(QVariantList rules READ rules NOTIFY changed)
    Q_PROPERTY(QVariantList contactPolicies READ contactPolicies NOTIFY changed)
    Q_PROPERTY(QVariantList identities READ identities NOTIFY changed)
    Q_PROPERTY(QStringList capabilities READ capabilities CONSTANT)
    Q_PROPERTY(QString error READ error NOTIFY changed)
  public:
    explicit Service(core::DsEngine& engine, QObject* parent = nullptr);
    ~Service() override;
    bool enabled() const;
    bool active() const;
    QVariantList pending() const;
    QVariantList history() const;
    QVariantList activity() const;
    QVariantList keys() const;
    QVariantList rules() const;
    QVariantList contactPolicies() const;
    QVariantList identities() const;
    QStringList capabilities() const;
    QString error() const { return error_; }
    void setUiAvailable(bool available);
    void setContactDecision(std::function<bool(int, bool)> fn) { contact_decision_ = std::move(fn); }
    bool configure(bool enabled, const QString& address, int port, int timeout);
    void stop();
    // Authentication executes on HTTP workers; its snapshot contains verifiers
    // only. Actual access is checked again on the Qt thread at every dispatch.
    QString authenticate(const std::string& bearer);
    bool reserveDispatch();
    void requestActivity();
    void submit(QString key, QString tool, QJsonObject args, std::shared_ptr<Reply> reply);
#ifdef DARKSPEAK_WITH_TEST_MCP
    void setTestSupport(TestSupport* support) { test_support_ = support; }
    bool testSupportActive() const { return test_support_ != nullptr; }
#endif
    Q_INVOKABLE QVariantMap createKey(const QString& name, const QStringList& identities,
                                      const QStringList& capabilities, qint64 expires = 0);
    Q_INVOKABLE bool updateKey(const QString& id, const QString& name, const QStringList& identities,
                               const QStringList& capabilities, qint64 expires, bool enabled);
    Q_INVOKABLE bool revokeKey(const QString& id);
    Q_INVOKABLE bool removeRule(const QString& id);
    Q_INVOKABLE bool setRule(const QString& key, const QString& identity, const QString& tool,
                             const QString& contact, const QString& decision);
    Q_INVOKABLE bool setContactPolicy(const QString& identity, const QString& contact,
                                      const QString& permission, const QString& decision);
    Q_INVOKABLE bool decide(const QString& id, const QString& decision, const QString& scope);
    Q_INVOKABLE QStringList contactsForIdentity(const QString& identity) const;
  signals:
    void changed();

  private:
    struct Pending {
        QString id, key, identity, tool;
        QStringList targets;
        QJsonObject args;
        qint64 deadline = 0;
        std::weak_ptr<Reply> reply;
    };
    struct Credential {
        QString id;
        QByteArray verifier;
        qint64 expires;
    };
    struct Event {
        quint64 sequence;
        QJsonObject data;
        qsizetype bytes;
        QStringList audience;
    };
    struct EventView {
        QString key;
        QStringList identities;
        qint64 expires;
        bool enabled;
    };
    void refreshCredentials();
    void authorize(const QString& key, const Tool& tool, const QString& identity,
                   const QStringList& targets) const;
    Evaluation approval(const QString& key, const Tool& tool, const QString& identity,
                        const QStringList& targets) const;
    QStringList validate(const Tool& tool, const QString& identity, QJsonObject& args, bool resolve = true,
                         const QString& key = {}) const;
    QJsonObject execute(const Tool& tool, const QString& identity, const QJsonObject& args,
                        const QString& operation = {}, const QString& key = {}, qint64 deadline = 0);
    QJsonObject status(const QString& key, const QString& id) const;
    void run(const QString& id);
    void finish(const QString& id, const QString& state, QJsonObject result = {});
    void invalidate(const std::function<bool(const Pending&)>& affected);
    void tick();
    void event(const QString& type, const QString& key, const QString& identity, const QString& tool,
               const QString& operation, const QStringList& targets = {});
    QJsonObject events(const QString& key, const QString& identity, const QJsonObject& args);
    bool eventTargetsVisible(const QJsonObject& data) const;
    void resetEventAuthorization();
    QByteArray mac(const QByteArray& data) const;
    QString seal(const QString& key, const QString& identity, const QString& domain, quint64 value) const;
    quint64 open(const QString& token, const QString& key, const QString& identity,
                 const QString& domain) const;
    void prune();
    QString retryEpoch();
    bool fail(const QString& code);
    core::DsEngine& engine_;
    Store store_;
    std::unique_ptr<Transport> transport_;
    std::function<bool(int, bool)> contact_decision_;
    QHash<QString, Pending> pending_;
    std::deque<Event> events_;
    QList<EventView> event_views_;
    QHash<QString, QHash<QString, quint64>> dropped_events_;
    QString event_authorization_;
    std::deque<QVariantMap> history_;
    qsizetype event_bytes_ = 0;
    quint64 sequence_ = 0;
    QString run_id_, error_, address_;
    int port_ = 0, timeout_ = 60;
    bool ui_available_ = false, stopped_ = false;
    QElapsedTimer clock_;
    qint64 last_activity_ = -3000, rate_window_ = 0;
    QHash<QString, int> calls_;
    QTimer timer_;
    QByteArray fingerprint_key_;
    mutable std::mutex credentials_mutex_;
    QList<Credential> credentials_;
    QSet<QString> used_keys_;
    QSet<QObject*> watched_contacts_;
    qint64 last_usage_write_ = 0;
    std::atomic_int dispatches_{0};
    std::atomic_bool activity_notification_{false};
    bool notified_active_ = false;
    qint64 notified_second_ = -1;
#ifdef DARKSPEAK_WITH_TEST_MCP
    TestSupport* test_support_ = nullptr;
#endif
};
} // namespace ds::mcp
