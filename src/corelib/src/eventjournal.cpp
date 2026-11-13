#include "ds/eventjournal.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <limits>
#include "logfault/logfault.h"

namespace ds::core {
namespace {
const char* select_rows = "SELECT id,version,timestamp,precision,kind,identity,contact,direction,outcome,object,repeat_count,window_start,window_end,previous_hash,hash,hash_version,filename FROM event";
QVariantMap readRow(const QSqlQuery& query) {
    static const QStringList names = {"id", "version", "timestamp", "precision", "kind", "identity", "contact", "direction", "outcome", "object", "repeat_count", "window_start", "window_end", "previous_hash", "hash", "hash_version", "filename"};
    QVariantMap row;
    for (int i = 0; i < names.size(); ++i) row.insert(names[i], query.value(i));
    return row;
}
QString boundedFilename(const QString& filename) {
    if (filename.isEmpty() || filename.size() > 255 || filename.toUtf8().size() > 255
        || filename == QStringLiteral(".") || filename == QStringLiteral("..")) return {};
    for (const auto c : filename) {
        if (c == '/' || c == '\\' || c.category() == QChar::Other_Control
            || c.category() == QChar::Other_Format) return {};
    }
    return filename;
}
QString kindName(int kind) {
    using E = EventJournal;
    switch (kind) {
    case E::RecordingEnabled: return E::tr("Event recording enabled");
    case E::RecordingDisabled: return E::tr("Event recording disabled");
    case E::Cleared: return E::tr("Events cleared; new chain started");
    case E::IdentityCreated: return E::tr("Identity created");
    case E::IdentityModified: return E::tr("Identity modified");
    case E::IdentityDeleted: return E::tr("Identity deleted");
    case E::ContactRequest: return E::tr("Contact request");
    case E::ContactDecision: return E::tr("Contact request decision");
    case E::Message: return E::tr("Message");
    case E::MessageReceipt: return E::tr("Message receipt");
    case E::FileOffer: return E::tr("File offer");
    case E::TransferState: return E::tr("File transfer");
    case E::AvatarChanged: return E::tr("Avatar changed");
    case E::AvatarRequest: return E::tr("Avatar request");
    case E::BioRequest: return E::tr("Bio request");
    case E::BioResponse: return E::tr("Bio response");
    case E::UserInfo: return E::tr("Profile information");
    case E::PolicyRejection: return E::tr("Content rejected by security policy");
    case E::RateLimited: return E::tr("Security rate limit applied");
    case E::ProtocolRejection: return E::tr("Protocol input rejected");
    case E::AvatarTransfer: return E::tr("Avatar transfer");
    case E::Aggregated: return E::tr("Additional events suppressed by journal limits");
    default: return E::tr("Unknown event");
    }
}
QString outcomeName(int outcome) {
    using E = EventJournal;
    switch (outcome) {
    case E::Success: return E::tr("Succeeded"); case E::Rejected: return E::tr("Rejected");
    case E::Failed: return E::tr("Failed"); case E::Pending: return E::tr("Pending");
    case E::Accepted: return E::tr("Approved"); default: return {};
    }
}
}
bool EventJournal::validPrecision(int seconds) {
    return seconds == 0 || seconds == 60 || seconds == 300 || seconds == 1800 || seconds == 3600;
}
EventJournal::EventJournal(QSqlDatabase db, QSettings& settings, QObject* parent)
    : QAbstractListModel(parent), db_(db), settings_(settings) {
    clock_.start();
    record_filenames_ = settings.value("events/recordFilenames", true).toBool();
    bool valid_precision = false;
    precision_ = settings.value("events/precisionSeconds", 60).toInt(&valid_precision);
    if (!valid_precision || !validPrecision(precision_)) precision_ = 60;
    const auto enabled_value = settings.value("events/enabled", false).toString().trimmed().toLower();
    enabled_ = enabled_value == QStringLiteral("true") || enabled_value == QStringLiteral("1");
    summary_timer_.setSingleShot(true);
    summary_timer_.setInterval(summary_interval_ms);
    connect(&summary_timer_, &QTimer::timeout, this, [this] { flush(); });
    refresh_timer_.setSingleShot(true);
    refresh_timer_.setInterval(250);
    connect(&refresh_timer_, &QTimer::timeout, this, [this] { if (active_ && follow_) latest(); });
    connect(this, &EventJournal::journalChanged, this, [this] {
        if (active_ && follow_ && !refresh_timer_.isActive()) refresh_timer_.start();
    });
    verify_timer_.setSingleShot(true);
    connect(&verify_timer_, &QTimer::timeout, this, &EventJournal::verifyChunk);
    if (enabled_) {
        if (!ensureSchema() || (stored_rows_ == 0 && !write(RecordingEnabled, 0, 0, Local, Success)))
            enabled_ = false;
    }
}
EventJournal::~EventJournal() { if (enabled_) flush(); }
bool EventJournal::fail(const QString& error) {
    if (error_ == error) return false;
    error_ = error;
    LFLOG_ERROR << "Event journal operation failed";
    emit errorChanged();
    return false;
}
bool EventJournal::ensureSchema() {
    if (ready_) return true;
    if (!db_.transaction()) return fail(tr("Unable to begin event database upgrade."));
    QSqlQuery query(db_);
    const auto upgrade_failed = [this] {
        db_.rollback(); return fail(tr("Unable to upgrade the event database."));
    };
    if (!query.exec("CREATE TABLE IF NOT EXISTS event_schema(version INTEGER NOT NULL CHECK(version=2))")
        || !query.exec("INSERT INTO event_schema SELECT 2 WHERE NOT EXISTS(SELECT 1 FROM event_schema)")
        || !query.exec("SELECT version FROM event_schema") || !query.next()) return upgrade_failed();
    const auto version = query.value(0).toInt();
    if ((version != 1 && version != 2) || query.next()) {
        db_.rollback(); return fail(tr("Unsupported event database version."));
    }
    query.finish();
    if (version == 1) {
        // Extend in place: old rows retain their original canonical hashes.
        for (const auto* sql : {
            "ALTER TABLE event ADD COLUMN hash_version INTEGER NOT NULL DEFAULT 1 CHECK(hash_version IN(1,2))",
            "ALTER TABLE event ADD COLUMN filename TEXT CHECK(filename IS NULL OR length(CAST(filename AS BLOB))<=255)",
            "DROP TABLE event_schema",
            "CREATE TABLE event_schema(version INTEGER NOT NULL CHECK(version=2))",
            "INSERT INTO event_schema VALUES(2)"}) {
            if (!query.exec(QLatin1String(sql))) return upgrade_failed();
        }
    }
    const QStringList statements = {
        "CREATE TABLE IF NOT EXISTS event (id INTEGER PRIMARY KEY AUTOINCREMENT, version INTEGER NOT NULL CHECK(version=1), timestamp INTEGER NOT NULL, precision INTEGER NOT NULL, kind INTEGER NOT NULL, identity INTEGER, contact INTEGER, direction INTEGER NOT NULL, outcome INTEGER NOT NULL, object BLOB NOT NULL CHECK(length(object)<=64), repeat_count INTEGER CHECK(repeat_count>0), window_start INTEGER, window_end INTEGER, previous_hash BLOB NOT NULL CHECK(length(previous_hash)=32), hash BLOB NOT NULL CHECK(length(hash)=32), hash_version INTEGER NOT NULL DEFAULT 1 CHECK(hash_version IN(1,2)), filename TEXT CHECK(filename IS NULL OR length(CAST(filename AS BLOB))<=255))",
        "CREATE INDEX IF NOT EXISTS ix_event_identity ON event(identity,id)",
        "CREATE INDEX IF NOT EXISTS ix_event_contact ON event(contact,id)"
    };
    for (const auto& sql : statements) if (!query.exec(sql)) return upgrade_failed();
    query.finish();
    if (!db_.commit()) { db_.rollback(); return fail(tr("Unable to commit the event database upgrade.")); }
    if (!query.exec("SELECT COUNT(*) FROM event") || !query.next())
        return fail(tr("Unable to read the event journal size."));
    stored_rows_ = query.value(0).toLongLong();
    ready_ = true;
    LFLOG_DEBUG << "Event database schema version 2 ready";
    return true;
}
qint64 EventJournal::rounded(qint64 seconds) const {
    return precision_ ? ((seconds + precision_ - 1) / precision_) * precision_ : seconds;
}
QByteArray EventJournal::rowHash(const QVariantMap& row, const QByteArray& previous) {
    QByteArray canonical;
    QDataStream stream(&canonical, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    const auto hash_version = row.value("hash_version", 1).toInt();
    stream << (hash_version == 2 ? QByteArray("DarkSpeak event chain v2") : QByteArray("DarkSpeak event chain v1")) << previous;
    for (const auto* key : {"id", "version", "timestamp", "precision", "kind", "direction", "outcome"})
        stream << qint64(row.value(QLatin1String(key)).toLongLong());
    for (const auto* key : {"identity", "contact"}) {
        const auto value = row.value(QLatin1String(key));
        stream << bool(!value.isNull()) << qint64(value.toLongLong());
    }
    const auto object = row.value("object").toByteArray();
    stream << (object.isEmpty() ? QByteArray("") : object);
    for (const auto* key : {"repeat_count", "window_start", "window_end"}) {
        const auto value = row.value(QLatin1String(key));
        stream << bool(!value.isNull()) << qint64(value.toLongLong());
    }
    if (hash_version == 2) {
        const auto filename = row.value("filename");
        const auto bytes = filename.toString().toUtf8();
        stream << bool(!filename.isNull()) << (bytes.isEmpty() ? QByteArray("") : bytes);
    }
    return QCryptographicHash::hash(canonical, QCryptographicHash::Sha256);
}
bool EventJournal::write(Kind kind, qint64 identity, qint64 contact, Direction direction,
                         Outcome outcome, const QByteArray& object, qint64 count, qint64 start, qint64 end, const QString& filename) {
    if (!ensureSchema()) return false;
    // Reserve the final row for the explicit disable marker.
    if (stored_rows_ >= maximum_rows - (kind == RecordingDisabled ? 0 : 1))
        return fail(tr("The event journal is full. Clear events to resume recording; existing events have been preserved."));
    QSqlQuery query(db_);
    if (!query.exec("SAVEPOINT append_event")) return fail(tr("Unable to begin writing an event."));
    const auto rollback = [&query] {
        query.exec("ROLLBACK TO append_event"); query.exec("RELEASE append_event");
    };
    QByteArray previous(32, '\0');
    if (!query.exec("SELECT hash FROM event ORDER BY id DESC LIMIT 1")) {
        rollback(); return fail(tr("Unable to read the event chain."));
    }
    if (query.next()) previous = query.value(0).toByteArray();
    query.finish();
    if (previous.size() != 32) { rollback(); return fail(tr("Invalid event chain tail.")); }
    const auto stored_filename = record_filenames_ && !count && (kind == FileOffer || kind == TransferState)
        ? boundedFilename(filename) : QString{};
    QVariantMap row{{"hash_version", 2}, {"filename", stored_filename.isNull() ? QVariant{} : QVariant(stored_filename)}, {"version", 1}, {"timestamp", rounded(QDateTime::currentSecsSinceEpoch())},
        {"precision", precision_}, {"kind", int(kind)}, {"identity", identity > 0 ? QVariant(identity) : QVariant{}},
        {"contact", contact > 0 ? QVariant(contact) : QVariant{}}, {"direction", int(direction)},
        {"outcome", int(outcome)}, {"object", object.isNull() ? QByteArray("") : object},
        {"repeat_count", count ? QVariant(count) : QVariant{}},
        {"window_start", count ? QVariant(rounded(start)) : QVariant{}},
        {"window_end", count ? QVariant(rounded(end)) : QVariant{}}, {"previous_hash", previous},
        {"hash", QByteArray(32, '\0')}};
    query.prepare("INSERT INTO event(version,timestamp,precision,kind,identity,contact,direction,outcome,object,repeat_count,window_start,window_end,previous_hash,hash,hash_version,filename) VALUES(:version,:timestamp,:precision,:kind,:identity,:contact,:direction,:outcome,:object,:repeat_count,:window_start,:window_end,:previous_hash,:hash,:hash_version,:filename)");
    for (auto it = row.cbegin(); it != row.cend(); ++it) query.bindValue(":" + it.key(), it.value());
    if (!query.exec()) { rollback(); return fail(tr("Unable to append an event.")); }
    row.insert("id", query.lastInsertId());
    const auto hash = rowHash(row, previous);
    query.prepare("UPDATE event SET hash=:hash WHERE id=:id");
    query.bindValue(":hash", hash); query.bindValue(":id", row.value("id"));
    if (!query.exec() || !query.exec("RELEASE append_event")) { rollback(); return fail(tr("Unable to commit an event.")); }
    ++stored_rows_;
    if (!error_.isEmpty()) { error_.clear(); emit errorChanged(); }
    emit journalChanged();
    return true;
}
bool EventJournal::configure(bool enabled, int precision, bool record_filenames) {
    if (!validPrecision(precision)) return false;
    if (enabled == enabled_ && precision == precision_ && record_filenames == record_filenames_) return true;
    // Close summaries under their original precision before changing it.
    if (enabled_ && !flush() && enabled) return false;
    const auto old_precision = precision_;
    precision_ = precision;
    if (enabled != enabled_) {
        if (!write(enabled ? RecordingEnabled : RecordingDisabled, 0, 0, Local, Success) && enabled) {
            precision_ = old_precision; return false;
        }
        enabled_ = enabled;
    }
    record_filenames_ = record_filenames;
    buckets_.clear(); overflow_ = {}; summary_timer_.stop(); global_tokens_ = global_burst; global_tick_ = clock_.elapsed();
    emit configurationChanged();
    return true;
}
void EventJournal::append(Kind kind, qint64 identity, qint64 contact, Direction direction,
                          Outcome outcome, const QByteArray& object, const QString& filename) {
    if (operational_enabled_ && kind >= IdentityCreated && kind < Aggregated
        && direction >= Local && direction <= Outgoing && outcome >= None && outcome <= Accepted)
        emit operationalEvent(int(kind), identity, contact, int(outcome));
    if (!enabled_ || kind < IdentityCreated || kind >= Aggregated || object.size() > 64
        || direction < Local || direction > Outgoing || outcome < None || outcome > Accepted) return;
    // Only local user actions create/modify/delete our identities. Remote
    // traffic must not crowd those records out of the global activity budget.
    if (direction == Local && kind >= IdentityCreated && kind <= IdentityDeleted) {
        write(kind, identity, contact, direction, outcome, object);
        return;
    }
    const auto tick = clock_.elapsed();
    const auto now = QDateTime::currentSecsSinceEpoch();
    global_tokens_ = std::min(double(global_burst), global_tokens_ + (tick - global_tick_) / 500.0);
    global_tick_ = tick;
    const Key key{int(kind), int(direction), int(outcome), std::max(qint64(0), identity), std::max(qint64(0), contact)};
    if (buckets_.size() == max_buckets && tick - prune_tick_ >= summary_interval_ms) {
        prune_tick_ = tick;
        std::erase_if(buckets_, [tick](const auto& entry) {
            return entry.second.count == 0 && tick - entry.second.tick >= summary_interval_ms;
        });
    }
    auto it = buckets_.find(key);
    if (it == buckets_.end() && buckets_.size() < max_buckets) it = buckets_.emplace(key, Bucket{double(bucket_burst), tick}).first;
    auto& bucket = it == buckets_.end() ? overflow_ : it->second;
    bucket.tokens = std::min(double(bucket_burst), bucket.tokens + (tick - bucket.tick) / 30000.0);
    bucket.tick = tick;
    if (it != buckets_.end() && global_tokens_ >= 1 && bucket.tokens >= 1) {
        --global_tokens_; --bucket.tokens;
        if (write(kind, key.identity, key.contact, direction, outcome, object, 0, 0, 0, filename)) return;
    }
    if (!bucket.count) bucket.start = now;
    bucket.end = now;
    if (bucket.count < std::numeric_limits<qint64>::max()) ++bucket.count;
    if (!summary_timer_.isActive()) summary_timer_.start();
}
bool EventJournal::flush() {
    summary_timer_.stop();
    const bool pending = overflow_.count || std::any_of(buckets_.begin(), buckets_.end(),
        [](const auto& entry) { return entry.second.count != 0; });
    if (!pending) return true;
    if (!ensureSchema()) { summary_timer_.start(); return false; }
    // One durable commit for the bounded batch, not up to 257 disk syncs on
    // the UI/network thread. Do not discard counters before that commit.
    QSqlQuery transaction(db_);
    if (!transaction.exec("SAVEPOINT summarize_events")) {
        summary_timer_.start(); return fail(tr("Unable to begin writing event summaries."));
    }
    const auto old_count = stored_rows_;
    bool ok = true;
    for (const auto& [key, bucket] : buckets_) {
        if (bucket.count && !write(Kind(key.kind), key.identity, key.contact, Direction(key.direction),
                Outcome(key.outcome), {}, bucket.count, bucket.start, bucket.end)) {
            ok = false; break;
        }
    }
    if (ok && overflow_.count)
        ok = write(Aggregated, 0, 0, Local, None, {}, overflow_.count, overflow_.start, overflow_.end);
    if (ok && !transaction.exec("RELEASE summarize_events"))
        ok = fail(tr("Unable to commit event summaries."));
    if (!ok) {
        transaction.exec("ROLLBACK TO summarize_events"); transaction.exec("RELEASE summarize_events");
        stored_rows_ = old_count;
        summary_timer_.start(); return false;
    }
    for (auto& [key, bucket] : buckets_) bucket.count = 0;
    overflow_.count = 0;
    return true;
}
void EventJournal::activate(bool active) { active_ = active; if (active) latest(); }
int EventJournal::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : rows_.size(); }
QVariant EventJournal::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size() || role != Row) return {};
    auto row = rows_[index.row()];
    row.insert("title", kindName(row.value("kind").toInt()));
    row.insert("result", outcomeName(row.value("outcome").toInt()));
    const auto direction = row.value("direction").toInt();
    row.insert("flow", direction == Incoming ? tr("Incoming") : direction == Outgoing ? tr("Outgoing") : tr("Local"));
    row.insert("time", QDateTime::fromSecsSinceEpoch(row.value("timestamp").toLongLong()).toLocalTime());
    row.insert("object", row.value("object").toByteArray().toHex());
    row.insert("hash", row.value("hash").toByteArray().toHex());
    row.insert("previous_hash", row.value("previous_hash").toByteArray().toHex());
    return row;
}
void EventJournal::latest() { follow_ = true; page(0, false); }
void EventJournal::older() { if (!rows_.isEmpty()) { follow_ = false; page(rows_.last().value("id").toLongLong(), false); } }
void EventJournal::newer() { if (!rows_.isEmpty()) { follow_ = false; page(rows_.first().value("id").toLongLong(), true); } }
void EventJournal::page(qint64 cursor, bool ascending) {
    if (!ensureSchema()) return;
    QSqlQuery query(db_);
    query.prepare(QString::fromLatin1(select_rows) + (cursor ? (ascending ? " WHERE id>:cursor" : " WHERE id<:cursor") : "") + (ascending ? " ORDER BY id ASC LIMIT 100" : " ORDER BY id DESC LIMIT 100"));
    if (cursor) query.bindValue(":cursor", cursor);
    if (!query.exec()) { fail(tr("Unable to load events.")); return; }
    QList<QVariantMap> rows;
    while (query.next()) rows.append(readRow(query));
    if (ascending) std::reverse(rows.begin(), rows.end());
    query.finish();
    has_older_ = has_newer_ = false;
    if (!rows.isEmpty()) {
        query.prepare("SELECT EXISTS(SELECT 1 FROM event WHERE id<:low),EXISTS(SELECT 1 FROM event WHERE id>:high)");
        query.bindValue(":low", rows.last().value("id")); query.bindValue(":high", rows.first().value("id"));
        if (query.exec() && query.next()) { has_older_ = query.value(0).toBool(); has_newer_ = query.value(1).toBool(); }
    }
    beginResetModel(); rows_ = std::move(rows); endResetModel(); emit pageChanged();
}
bool EventJournal::clear() {
    if (!ensureSchema()) return false;
    QSqlQuery query(db_);
    if (!query.exec("SAVEPOINT clear_events")) return fail(tr("Unable to clear events."));
    const auto old_count = stored_rows_;
    stored_rows_ = 0;
    if (!query.exec("DELETE FROM event") || !write(Cleared, 0, 0, Local, Success)
        || !query.exec("RELEASE clear_events")) {
        query.exec("ROLLBACK TO clear_events"); query.exec("RELEASE clear_events");
        stored_rows_ = old_count;
        return fail(tr("Unable to clear events."));
    }
    buckets_.clear(); overflow_ = {}; summary_timer_.stop();
    verifying_ = false; verify_timer_.stop(); verification_.clear(); emit verificationChanged();
    emit journalChanged(); latest(); return true;
}
void EventJournal::verify() {
    if (verifying_ || !ensureSchema()) return;
    QSqlQuery query(db_);
    if (!query.exec("SELECT COALESCE(MAX(id),0) FROM event") || !query.next()) { fail(tr("Unable to verify events.")); return; }
    verify_tail_ = query.value(0).toLongLong(); verify_cursor_ = 0; verify_previous_ = QByteArray(32, '\0');
    verifying_ = true; verification_ = tr("Checking event chain…"); emit verificationChanged(); verify_timer_.start(0);
}
void EventJournal::verifyChunk() {
    QSqlQuery query(db_);
    query.prepare(QString::fromLatin1(select_rows) + " WHERE id>:cursor AND id<=:tail ORDER BY id LIMIT 100");
    query.bindValue(":cursor", verify_cursor_); query.bindValue(":tail", verify_tail_);
    if (!query.exec()) { verifying_ = false; verification_ = tr("Unable to read the event chain."); emit verificationChanged(); return; }
    int count = 0;
    while (query.next()) {
        const auto row = readRow(query); ++count;
        const auto hash_version = row.value("hash_version").toInt();
        if (row.value("version").toInt() != 1 || (hash_version != 1 && hash_version != 2)
            || (hash_version == 1 && !row.value("filename").isNull()) || row.value("previous_hash").toByteArray() != verify_previous_ || row.value("hash").toByteArray() != rowHash(row, verify_previous_)) {
            verifying_ = false; verification_ = tr("Event chain mismatch at row %1.").arg(row.value("id").toString()); emit verificationChanged(); return;
        }
        verify_cursor_ = row.value("id").toLongLong(); verify_previous_ = row.value("hash").toByteArray();
    }
    if (verify_cursor_ == verify_tail_) {
        verifying_ = false; verification_ = tr("Event chain is internally consistent through row %1.").arg(verify_tail_); emit verificationChanged();
    } else if (!count) {
        verifying_ = false; verification_ = tr("Event chain changed during verification. Run the check again."); emit verificationChanged();
    } else verify_timer_.start(0);
}
}
