#pragma once
#include <dsfilemanager/filemanager.h>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <limits>
#include <QSet>
#include <QTimeZone>
#include <unistd.h>

namespace dsfm::detail {
struct Failure { Error error; };
[[noreturn]] inline void fail(ErrorCode code, int native_code = 0) { throw Failure{{code, native_code}}; }
inline void require(bool condition, ErrorCode code) { if (!condition) fail(code); }
void ioCheck(bool condition);
struct Fd {
    int value = -1;
    Fd() = default;
    explicit Fd(int fd) : value(fd) { ioCheck(fd >= 0); }
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept { if (value >= 0) ::close(value); value = std::exchange(other.value, -1); return *this; }
    Fd(const Fd&) = delete;
    explicit operator bool() const { return value >= 0; }
};
struct Identity {
    quint64 dev = 0, ino = 0, size = 0;
    qint64 modified = 0;
    bool directory = false;
};
Identity identity(int fd, bool directory = false);
Fd openBeneath(int dir, const QString& path, int flags, int mode = 0);
void makeDirectory(int root, const QString& path);
void renameSafe(int root, const QString& from, const QString& to, bool replace = false);
void unlinkSafe(int root, const QString& path, bool directory = false);
void syncDirectory(int root, const QString& path);
QString parentPath(QString path);
QVector<std::shared_ptr<Fd>> lockRoots(int root, bool exclusive = true);
Fd openRoot(const QString& path, bool create, bool private_root = true);
QString uuid(QUuid id);
QUuid uuid(const QVariant& value);
void fault(const char* point);

void registerSqliteVfs();

class Catalog {
public:
    Catalog(const QString& path, quint64 max_bytes, bool create);
    ~Catalog();
    QSqlQuery query(const QString& sql, const QVariantList& args = {});
    QVariant scalar(const QString& sql, const QVariantList& args = {});
    void begin();
    void commit();
    void rollback();
    QString meta(const QString& key);
    void setMeta(const QString& key, const QString& value);
    quint64 generation();
    quint64 bump();
    FileInfo entry(QUuid id);
    QString path(QUuid id, PhysicalNaming naming, quint32 depth);
    PendingInfo pending(PendingId id);
    QVector<ByteRange> ranges(PendingId id, RangeKind kind);
    void putRanges(PendingId id, RangeKind kind, const QVector<ByteRange>& ranges);
    QSqlDatabase db;
private:
    QString connection_;
};
struct Transaction {
    Catalog& db;
    bool complete = false;
    explicit Transaction(Catalog& catalog) : db(catalog) { db.begin(); }
    ~Transaction() { if (!complete) db.rollback(); }
    void commit() { db.commit(); complete = true; }
};
struct Namespace {
    NamespaceConfig config;
    DirectoryId root_id;
    Fd root;
    Fd internal;
    Fd owner_lock;
    QVector<std::shared_ptr<Fd>> locks;
    std::mutex mutex;
    QSet<PendingId> active_writers;
    QHash<FileId, quint32> readers;
    quint64 handles = 0;
    quint64 untracked_staging = 0;
    bool staging_scan_truncated = false;
    QString catalogPath() const { return QStringLiteral("/proc/self/fd/%1/catalog.db").arg(internal.value); }
};
QVector<ByteRange> addRange(QVector<ByteRange> ranges, ByteRange range);
QVector<ByteRange> subtractRange(const QVector<ByteRange>& ranges, ByteRange range);
bool covers(const QVector<ByteRange>& ranges, ByteRange range);
QJsonObject limitsJson(const NamespaceLimits& limits);
NamespaceLimits readLimits(const QJsonObject& object);
void validateLimits(const NamespaceLimits& limits);
void cancelCheck(const CancellationToken& cancel);
void recover(Namespace& ns, Catalog& db);
quint64 charged(Catalog& db);
void checkQuota(Namespace& ns, Catalog& db, quint64 reserve, quint64 old_reserve = 0);
void validateEntry(Namespace& ns, Catalog& db, const FileInfo& entry);
QString pendingPath(PendingId id);
QByteArray verifyContent(Namespace& ns, Catalog& db, const PendingInfo& pending, const QString& path, CancellationToken cancel);
MutationResult applyOperation(Namespace& ns, Catalog& db, QJsonObject operation);
QByteArray cursor(quint64 generation, const QString& scope, const QString& last);
QString readCursor(const PageRequest& page, quint64 generation, const QString& scope, quint32 max_page);
} // namespace dsfm::detail
