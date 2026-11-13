#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QSettings>
#include <QSqlDatabase>
#include <QTimer>
#include <map>
#include <compare>
#include <QVariantMap>

namespace ds::core {
class PeerConnection;

// Metadata only, with optional bounded filenames. No message bodies or paths.
class EventJournal final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY configurationChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY pageChanged)
    Q_PROPERTY(bool hasNewer READ hasNewer NOTIFY pageChanged)
    Q_PROPERTY(QString verification READ verification NOTIFY verificationChanged)
    Q_PROPERTY(bool verifying READ verifying NOTIFY verificationChanged)
public:
    enum Kind {
        RecordingEnabled = 1, RecordingDisabled, Cleared,
        IdentityCreated, IdentityModified, IdentityDeleted, ContactRequest,
        ContactDecision, Message, MessageReceipt, FileOffer, TransferState,
        AvatarChanged, AvatarRequest, BioRequest, BioResponse, UserInfo,
        PolicyRejection, RateLimited, ProtocolRejection, AvatarTransfer, Aggregated
    };
    Q_ENUM(Kind)
    enum Direction { Local, Incoming, Outgoing };
    Q_ENUM(Direction)
    enum Outcome { None, Success, Rejected, Failed, Pending, Accepted };
    Q_ENUM(Outcome)
    enum Role { Row = Qt::UserRole + 1 };
    EventJournal(QSqlDatabase db, QSettings& settings, QObject* parent = nullptr);
    ~EventJournal() override;
    bool enabled() const { return enabled_; }
    void setOperationalCollectionEnabled(bool enabled) { operational_enabled_ = enabled; }
    bool operationalCollectionEnabled() const { return operational_enabled_; }
    QString error() const { return error_; }
    bool hasOlder() const { return has_older_; }
    bool hasNewer() const { return has_newer_; }
    QString verification() const { return verification_; }
    bool verifying() const { return verifying_; }
    bool configure(bool enabled, int precision, bool record_filenames = true);
    void append(Kind kind, qint64 identity = 0, qint64 contact = 0,
                Direction direction = Local, Outcome outcome = None,
                const QByteArray& object = {}, const QString& filename = {});
    Q_INVOKABLE void activate(bool active);
    Q_INVOKABLE void latest();
    Q_INVOKABLE void older();
    Q_INVOKABLE void newer();
    Q_INVOKABLE bool clear();
    Q_INVOKABLE void verify();
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return {{Row, "event"}}; }
    static bool validPrecision(int seconds);
    static QByteArray rowHash(const QVariantMap& row, const QByteArray& previous);
    // Explicit local bounds, independent of peer-selected identifiers.
    static constexpr int maximum_rows = 500000;
    static constexpr int max_buckets = 256;
    static constexpr int global_burst = 120;
    static constexpr int bucket_burst = 20;
    static constexpr int summary_interval_ms = 30 * 60 * 1000;
signals:
    void configurationChanged();
    void errorChanged();
    void pageChanged();
    void verificationChanged();
    void journalChanged();
    // Bounded metadata producer, independent of optional persistent recording.
    void operationalEvent(int kind, qint64 identity, qint64 contact, int outcome);
private:
    struct Key {
        int kind, direction, outcome;
        qint64 identity, contact;
        auto operator<=>(const Key&) const = default;
    };
    struct Bucket { double tokens = bucket_burst; qint64 tick = 0, count = 0, start = 0, end = 0; };
    bool ensureSchema();
    bool write(Kind kind, qint64 identity, qint64 contact, Direction direction,
               Outcome outcome, const QByteArray& object = {}, qint64 count = 0,
               qint64 start = 0, qint64 end = 0, const QString& filename = {});
    bool fail(const QString& error);
    bool flush();
    void page(qint64 cursor, bool ascending);
    void verifyChunk();
    qint64 rounded(qint64 seconds) const;
    QSqlDatabase db_;
    QSettings& settings_;
    bool operational_enabled_ = false;
    bool enabled_ = false, ready_ = false, active_ = false, record_filenames_ = true;
    bool has_older_ = false, has_newer_ = false, follow_ = true;
    int precision_ = 60;
    qint64 stored_rows_ = 0;
    QString error_, verification_;
    bool verifying_ = false;
    qint64 verify_cursor_ = 0, verify_tail_ = 0;
    QByteArray verify_previous_;
    QElapsedTimer clock_;
    double global_tokens_ = global_burst;
    qint64 global_tick_ = 0, prune_tick_ = 0;
    std::map<Key, Bucket> buckets_;
    Bucket overflow_;
    QTimer summary_timer_, refresh_timer_, verify_timer_;
    QList<QVariantMap> rows_;
};
// Protocol hooks keep payloads and diagnostic strings out of the journal.
void recordPeerEvent(const PeerConnection& peer, EventJournal::Kind kind,
                     EventJournal::Direction direction, EventJournal::Outcome outcome = EventJournal::None,
                     const QByteArray& object = {});
void recordSecurityLimit(EventJournal::Direction direction);
}
