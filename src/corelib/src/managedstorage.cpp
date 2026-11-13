#include "ds/managedstorage.h"
#include "ds/resourcelimits.h"
#include "ds/avatarutil.h"
#include "ds/mediapreview.h"
#include "ds/ffmpeg.h"
#include "ds/file.h"
#include "logfault/logfault.h"
#include <QCoroSignal>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QPointer>
#include <QImageReader>
#include <QSqlQuery>
#include <QThreadPool>
#include <QTimer>
#include <coroutine>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ds::core {
namespace {
using StorageError = dsfm::Error;
using dsfm::ErrorCode;
template<class T> using Result = dsfm::Result<T>;
constexpr qsizetype io_bytes = 64 * 1024;
constexpr qsizetype avatar_bytes = 2 * 1024 * 1024;

// Only explicit local import/export uses this executor. Managed I/O uses the library executor.
struct LocalExecutor {
    QThreadPool pool;
    std::atomic_uint outstanding{0};
    LocalExecutor() { pool.setMaxThreadCount(2); pool.setExpiryTimeout(30000); }
};
LocalExecutor& executor() { static LocalExecutor value; return value; }
template<class T> struct DiskCall {
    struct State { std::function<Result<T>()> run; std::optional<Result<T>> result; };
    std::shared_ptr<State> state;
    bool await_ready() {
        auto& count = executor().outstanding;
        auto current = count.load();
        while (current < 32) {
            if (count.compare_exchange_weak(current, current + 1)) { return false; }
        }
        state->result.emplace(StorageError{ErrorCode::QueueFull});
        return true;
    }
    void await_suspend(std::coroutine_handle<> continuation) {
        auto* context = new QObject;
        executor().pool.start([state = state, context, continuation] {
            try { state->result.emplace(state->run()); }
            catch (...) { state->result.emplace(StorageError{ErrorCode::IoError}); }
            --executor().outstanding;
            QMetaObject::invokeMethod(context, [context, continuation] {
                context->deleteLater();
                continuation.resume();
            }, Qt::QueuedConnection);
        });
    }
    Result<T> await_resume() { return std::move(*state->result); }
};
template<class T, class F> QCoro::Task<Result<T>> disk(F fn) {
    using Call = DiskCall<T>;
    auto state = std::make_shared<typename Call::State>();
    state->run = std::move(fn);
    co_return co_await Call{std::move(state)};
}
struct LocalFile {
    int fd = -1;
    struct stat snapshot{};
    ~LocalFile() { if (fd >= 0) ::close(fd); }
};
bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
}
QString uuid(QUuid id) { return id.toString(QUuid::WithoutBraces); }
} // namespace

ManagedStorage::Operation ManagedStorage::holdOperation() { ++active_; return Operation{this}; }
ManagedStorage::Operation::~Operation() { if (storage_) { --storage_->active_; emit storage_->drained(); } }

ManagedStorage::ManagedStorage(QSettings& settings, QObject* parent)
    : QObject(parent), settings_(settings) {
    const auto base = QFileInfo(settings.value("dbpath").toString()).absolutePath();
    configs_[Downloads].storage_root = QDir(settings.value("downloadLocation").toString()).absoluteFilePath("Managed-v1");
    configs_[Downloads].naming = settings.value("storage/originalFilenames", true).toBool()
        ? dsfm::PhysicalNaming::OriginalName : dsfm::PhysicalNaming::FileId;
    configs_[Private].storage_root = QDir(base).absoluteFilePath("managed-private-v1");
    configs_[Private].naming = dsfm::PhysicalNaming::FileId;
    for (int area = 0; area < 2; ++area) {
        auto& config = configs_[area];
        config.repair_permissions = settings.value("storage/repairPermissions", true).toBool();
        QSqlQuery query;
        query.prepare("SELECT id,naming FROM managed_namespace WHERE storage_root=:root");
        query.bindValue(":root", config.storage_root);
        if (!query.exec()) { initialization_error_ = StorageError{ErrorCode::IoError}; break; }
        if (query.next()) {
            config.id = query.value(0).toUuid();
            config.naming = dsfm::PhysicalNaming(query.value(1).toInt());
            if (area == Downloads) {
                settings.setValue("storage/downloadsInitialized", true);
                settings.setValue("storage/originalFilenames", config.naming == dsfm::PhysicalNaming::OriginalName);
            }
        } else config.id = QUuid::createUuid();
    }
    settings.sync();
    const auto limits = ResourceLimits::fromSettings(settings);
    auto& incoming = configs_[Downloads].limits;
    incoming.max_file_size = qMin(limits.acceptedFileBytes, limits.incomingDiskBytes);
    incoming.max_total_bytes = limits.incomingDiskBytes;
    incoming.max_staging_bytes = qMin(limits.unfinishedIncomingBytes, limits.incomingDiskBytes);
    incoming.max_pending = limits.retainedInterruptedTransfers;
    incoming.max_writers = qMin<quint32>(16, limits.activeFileTransfers);
    configs_[Downloads].limits_override = incoming;
    auto* gc = new QTimer(this);
    gc->setInterval(60000);
    connect(gc, &QTimer::timeout, this, [this] { if (ready_ && !stopping_) collectGarbage(); });
    gc->start();
}
QCoro::Task<Result<void>> ManagedStorage::ready() {
    if (stopping_) co_return StorageError{ErrorCode::ShuttingDown};
    if (ready_) co_return Result<void>{};
    if (initializing_) co_await qCoro(this, &ManagedStorage::initialized);
    if (ready_) co_return Result<void>{};
    if (initialization_error_) co_return *initialization_error_;
    initializing_ = true;
    QSqlQuery query;
    for (int area = 0; area < 2; ++area) {
        query.prepare("SELECT COUNT(*),COALESCE(MAX(id=:id),0) FROM managed_namespace");
        query.bindValue(":id", uuid(configs_[area].id));
        if (!query.exec() || !query.next()) { initialization_error_ = StorageError{ErrorCode::IoError}; break; }
        if (query.value(0).toInt() >= 32 && !query.value(1).toBool()) { initialization_error_ = StorageError{ErrorCode::QuotaExceeded}; break; }
        query.finish();
        query.prepare("INSERT OR IGNORE INTO managed_namespace(id,storage_root,naming,area) VALUES(:id,:root,:naming,:area)");
        query.bindValue(":id", uuid(configs_[area].id)); query.bindValue(":root", configs_[area].storage_root);
        query.bindValue(":naming", int(configs_[area].naming)); query.bindValue(":area", area);
        if (!query.exec()) { initialization_error_ = StorageError{ErrorCode::IoError}; break; }
    }
    struct Registered { dsfm::NamespaceConfig config; int area; bool created; };
    QVector<Registered> registered;
    if (!initialization_error_) {
        if (!query.exec("SELECT id,storage_root,naming,area,created FROM managed_namespace ORDER BY id LIMIT 33")) initialization_error_ = StorageError{ErrorCode::IoError};
        else while (query.next()) {
            const auto area = query.value(3).toInt();
            if (area < 0 || area > 1) { initialization_error_ = StorageError{ErrorCode::Corruption}; break; }
            auto config = configs_[area];
            config.id = query.value(0).toUuid(); config.storage_root = query.value(1).toString();
            config.naming = dsfm::PhysicalNaming(query.value(2).toInt());
            registered.push_back({config, area, query.value(4).toBool()});
        }
        query.finish();
        if (registered.size() > 32) initialization_error_ = StorageError{ErrorCode::QuotaExceeded};
    }
    for (const auto& item : registered) {
        if (initialization_error_) break;
        const auto config = item.config;
        const auto area_name = item.area == Downloads ? "downloads" : "private";
        LFLOG_DEBUG << "Opening managed storage namespace; area=" << area_name
                    << ", previously_created=" << item.created;
        LFLOG_TRACE << "Managed storage namespace root; area=" << area_name
                    << ", path=" << config.storage_root.toStdString();
        auto catalog_exists = co_await disk<bool>([config] {
            return Result<bool>{QFileInfo::exists(QDir(config.storage_root).filePath(".filemanager/catalog.db"))};
        });
        if (!catalog_exists) { initialization_error_ = catalog_exists.error(); break; }
        auto opened = (item.created || catalog_exists.value()) ? co_await manager_.openNamespace(config)
                                                               : co_await manager_.createNamespace(config);
        if (!opened) {
            initialization_error_ = opened.error();
            LFLOG_ERROR << "Managed storage namespace initialization failed; area=" << area_name
                        << ", operation=" << ((item.created || catalog_exists.value()) ? "open" : "create")
                        << ", code=" << int(opened.error().code)
                        << ", native=" << opened.error().native_code;
            break;
        }
        if (item.area == Downloads) download_namespaces_.insert(config.id);
        if (config.id == configs_[item.area].id) roots_[item.area] = opened.value().root;
        query.prepare("UPDATE managed_namespace SET created=1 WHERE id=:id"); query.bindValue(":id", uuid(config.id));
        if (!query.exec()) { initialization_error_ = StorageError{ErrorCode::IoError}; break; }
    }
    if (!initialization_error_) { settings_.setValue("storage/downloadsInitialized", true); settings_.sync(); }
    ready_ = !initialization_error_;
    if (ready_) {
        auto collected = co_await collectGarbage();
        if (!collected) LFLOG_WARN << "Managed storage cleanup deferred (code=" << int(collected.error().code) << ")";
    }
    initializing_ = false;
    if (!ready_) LFLOG_ERROR << "Managed storage unavailable (code=" << int(initialization_error_->code) << ")";
    emit initialized();
    emit drained();
    if (!ready_) { emit storageError(); co_return *initialization_error_; }
    LFLOG_DEBUG << "Managed file and media namespaces ready";
    co_return Result<void>{};
}
QCoro::Task<Result<ManagedStorage::Imported>> ManagedStorage::importFile(QString path, QString name, dsfm::CancellationToken cancel, std::function<bool(dsfm::PendingInfo)> reserved) {
    auto initialized = co_await ready();
    if (!initialized) co_return initialized.error();
    if (active_ >= 16) co_return StorageError{ErrorCode::QueueFull};
    ++active_;
    struct Guard { ManagedStorage* self; ~Guard() { --self->active_; emit self->drained(); } } guard{this};
    const auto max_size = configs_[Private].limits.max_file_size;
    auto opened = co_await disk<std::shared_ptr<LocalFile>>([path, max_size] {
        auto file = std::make_shared<LocalFile>();
        file->fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (file->fd < 0 || ::fstat(file->fd, &file->snapshot) || !S_ISREG(file->snapshot.st_mode)
            || file->snapshot.st_size < 0 || quint64(file->snapshot.st_size) > max_size)
            return Result<std::shared_ptr<LocalFile>>{StorageError{ErrorCode::InvalidArgument}};
        return Result<std::shared_ptr<LocalFile>>{file};
    });
    if (!opened) co_return opened.error();
    auto source = opened.value();
    dsfm::CreateOptions options{root(Private), name};
    options.content.expected_size = quint64(source->snapshot.st_size);
    auto created = co_await manager_.createFile(namespaceId(Private), options);
    if (!created) co_return created.error();
    auto pending = std::move(created).value();
    if (reserved && !reserved(pending.info)) { co_await pending.writer->abort(); co_return StorageError{ErrorCode::IoError}; }
    StorageError failure{ErrorCode::Cancelled};
    bool failed = false;
    quint64 offset = 0;
    QString mime_type;
    while (offset < *options.content.expected_size && !cancel.cancelled()) {
        auto read = co_await disk<QByteArray>([source, offset] {
            struct stat current{};
            if (::fstat(source->fd, &current) || !same(current, source->snapshot)) return Result<QByteArray>{StorageError{ErrorCode::ExternalModification}};
            QByteArray bytes(io_bytes, Qt::Uninitialized);
            const auto count = ::pread(source->fd, bytes.data(), bytes.size(), off_t(offset));
            if (count <= 0) return Result<QByteArray>{StorageError{ErrorCode::IoError}};
            bytes.resize(count);
            return Result<QByteArray>{std::move(bytes)};
        });
        if (!read) { failure = read.error(); failed = true; break; }
        if (!offset) {
            const auto detected = QMimeDatabase{}.mimeTypeForData(read.value());
            // Qt distinguishes codec-specific desktop MIME subtypes (for
            // example audio/x-vorbis+ogg). Advertise the standard HTTP container
            // type so an explicit audio/ogg preference accepts actual Ogg audio.
            if (detected.inherits(QStringLiteral("audio/ogg")))
                mime_type = QStringLiteral("audio/ogg");
            else if (detected.inherits(QStringLiteral("video/ogg")))
                mime_type = QStringLiteral("video/ogg");
            else
                mime_type = detected.name();
        }
        auto written = co_await pending.writer->writeAt(offset, read.value(), cancel);
        if (!written || written.value() != read.value().size()) { failure = written ? StorageError{ErrorCode::IoError} : written.error(); failed = true; break; }
        offset += quint64(written.value());
    }
    auto unchanged = co_await disk<bool>([source] {
        struct stat current{};
        return Result<bool>{!::fstat(source->fd, &current) && same(current, source->snapshot)};
    });
    if (cancel.cancelled() || failed || !unchanged || !unchanged.value()) {
        co_await pending.writer->abort();
        co_return failed ? failure : StorageError{cancel.cancelled() ? ErrorCode::Cancelled : ErrorCode::ExternalModification};
    }
    auto committed = co_await pending.writer->commit(cancel);
    if (!committed) {
        if (committed.error().code != ErrorCode::IndeterminateOperation) co_await pending.writer->abort();
        co_return committed.error();
    }
    auto info = co_await manager_.stat(namespaceId(Private), pending.info.file_id);
    if (!info) co_return info.error();
    if (mime_type.isEmpty()) mime_type = QStringLiteral("application/octet-stream");
    co_return Imported{namespaceId(Private), info.value(), mime_type};
}
QCoro::Task<Result<QString>> ManagedStorage::exportFile(dsfm::NamespaceId ns, dsfm::FileId id, QString name, dsfm::CancellationToken cancel) {
    auto initialized = co_await ready();
    if (!initialized) co_return initialized.error();
    if (export_slots_ >= 32 || active_ >= 16) co_return StorageError{ErrorCode::QuotaExceeded};
    auto normalized = dsfm::normalizeName(name);
    if (!normalized) co_return normalized.error();
    ++active_;
    struct Guard { ManagedStorage* self; ~Guard() { --self->active_; emit self->drained(); } } guard{this};
    auto info = co_await manager_.stat(ns, id);
    if (!info) co_return info.error();
    constexpr quint64 export_budget = 64ULL << 30;
    if (info.value().size > export_budget - export_bytes_ || export_slots_ >= 32) co_return StorageError{ErrorCode::QuotaExceeded};
    export_bytes_ += info.value().size; ++export_slots_;
    struct Reservation { ManagedStorage* self; quint64 bytes; bool retained = false; ~Reservation() { if (!retained) { self->export_bytes_ -= bytes; --self->export_slots_; } } } reservation{this, info.value().size};
    auto opened = co_await manager_.openRead(ns, id);
    if (!opened) co_return opened.error();
    auto reader = std::move(opened).value();
    // Exported copies are a bounded, explicit external-application boundary, never managed paths.
    auto directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/darkspeak-open-XXXXXX");
    if (!directory->isValid()) co_return StorageError{ErrorCode::StorageUnavailable};
    const auto path = directory->filePath(normalized.value());
    auto sink = std::make_shared<LocalFile>();
    auto created = co_await disk<void>([sink, path] {
        sink->fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        return sink->fd < 0 ? Result<void>{StorageError{ErrorCode::IoError}} : Result<void>{};
    });
    if (!created) co_return created.error();
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    quint64 offset = 0;
    std::optional<StorageError> failure;
    while (offset < info.value().size && !cancel.cancelled()) {
        auto read = co_await reader->readAt(offset, io_bytes, cancel);
        if (!read || read.value().isEmpty()) { failure = read ? StorageError{ErrorCode::IoError} : read.error(); break; }
        auto written = co_await disk<void>([sink, hash, bytes = read.value(), offset] {
            hash->addData(bytes);
            qsizetype done = 0;
            while (done < bytes.size()) {
                const auto count = ::pwrite(sink->fd, bytes.constData() + done, size_t(bytes.size() - done), off_t(offset + done));
                if (count <= 0) return Result<void>{StorageError{ErrorCode::IoError}};
                done += count;
            }
            return Result<void>{};
        });
        if (!written) { failure = written.error(); break; }
        offset += quint64(read.value().size());
    }
    co_await reader->close();
    if (failure) co_return *failure;
    if (cancel.cancelled()) co_return StorageError{ErrorCode::Cancelled};
    const auto digest = info.value().sha256;
    auto synced = co_await disk<void>([sink, hash, digest] {
        if (hash->result() != digest) return Result<void>{StorageError{ErrorCode::IntegrityError}};
        return ::fsync(sink->fd) ? Result<void>{StorageError{ErrorCode::IoError}} : Result<void>{};
    });
    if (!synced) co_return synced.error();
    exports_.push_back(std::move(directory));
    reservation.retained = true;
    co_return path;
}
QCoro::Task<Result<QByteArray>> ManagedStorage::readAsset(QString key) {
    auto initialized = co_await ready();
    if (!initialized) co_return initialized.error();
    if (active_ >= 64) co_return StorageError{ErrorCode::QueueFull};
    ++active_;
    struct Guard { ManagedStorage* self; QString key; bool owns = false; ~Guard() { if (owns) self->asset_writers_.remove(key); --self->active_; emit self->drained(); } } guard{this, key};
    while (asset_writers_.contains(key)) co_await qCoro(this, &ManagedStorage::drained);
    asset_writers_.insert(key); guard.owns = true;
    QSqlQuery query;
    query.prepare("SELECT file_id FROM media_asset WHERE asset_key=:key AND deleted=0");
    query.bindValue(":key", key);
    if (!query.exec()) co_return StorageError{ErrorCode::IoError};
    if (!query.next()) co_return StorageError{ErrorCode::MissingEntry};
    const auto id = query.value(0).toUuid();
    query.finish();
    auto info = co_await manager_.stat(namespaceId(Private), id);
    if (!info) co_return info.error();
    if (info.value().size > quint64(avatar_bytes)) co_return StorageError{ErrorCode::QuotaExceeded};
    auto opened = co_await manager_.openRead(namespaceId(Private), id);
    if (!opened) co_return opened.error();
    auto reader = std::move(opened).value();
    QByteArray bytes;
    while (quint64(bytes.size()) < info.value().size) {
        auto chunk = co_await reader->readAt(quint64(bytes.size()), io_bytes);
        if (!chunk || chunk.value().isEmpty()) { co_await reader->close(); co_return chunk ? StorageError{ErrorCode::IoError} : chunk.error(); }
        bytes.append(chunk.value());
    }
    co_await reader->close();
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != info.value().sha256) co_return StorageError{ErrorCode::IntegrityError};
    co_return bytes;
}
QCoro::Task<Result<void>> ManagedStorage::writeAsset(QString key, QByteArray bytes) {
    co_return co_await writeAssetImpl(std::move(key), std::move(bytes), false);
}
QCoro::Task<Result<void>> ManagedStorage::writeAssetImpl(QString key, QByteArray bytes, bool preview_lane) {
    if (bytes.size() > avatar_bytes || key.size() > 256) co_return StorageError{ErrorCode::QuotaExceeded};
    if (preview_lane && (!key.startsWith("preview/v1/") || bytes.size() > media_preview::max_encoded_bytes))
        co_return StorageError{ErrorCode::InvalidArgument};
    auto initialized = co_await ready();
    if (!initialized) co_return initialized.error();
    while (asset_writers_.contains(key)) co_await qCoro(this, &ManagedStorage::drained);
    // Preview waiters hold lifetime leases. They must not consume all ordinary
    // avatar-write admission slots and prevent the one serialized preview job
    // from draining. The private lane still has an absolute operation bound.
    if (active_ >= (preview_lane ? 64u : 16u)) co_return StorageError{ErrorCode::QueueFull};
    asset_writers_.insert(key);
    ++active_;
    struct Guard { ManagedStorage* self; QString key; ~Guard() { self->asset_writers_.remove(key); --self->active_; emit self->drained(); } } guard{this, key};
    QSqlQuery query;
    query.prepare("SELECT file_id, pending_id FROM media_asset WHERE asset_key=:key");
    query.bindValue(":key", key);
    if (!query.exec()) co_return StorageError{ErrorCode::IoError};
    QUuid old_id, old_pending;
    if (query.next()) { old_id = query.value(0).toUuid(); old_pending = query.value(1).toUuid(); }
    query.finish();
    if (!old_pending.isNull()) {
        auto aborted = co_await manager_.abortPending(namespaceId(Private), old_pending);
        if (!aborted && aborted.error().code != ErrorCode::MissingEntry) co_return aborted.error();
    }
    if (bytes.isEmpty()) {
        query.prepare("UPDATE media_asset SET deleted=1 WHERE asset_key=:key");
        query.bindValue(":key", key);
        if (!query.exec()) co_return StorageError{ErrorCode::IoError};
        if (!old_id.isNull()) {
            auto removed = co_await manager_.remove(namespaceId(Private), old_id);
            if (!removed && removed.error().code != ErrorCode::MissingEntry) co_return removed.error();
        }
        query.prepare("DELETE FROM media_asset WHERE asset_key=:key");
        query.bindValue(":key", key);
        if (!query.exec()) co_return StorageError{ErrorCode::IoError};
        co_return Result<void>{};
    }
    dsfm::ContentConstraints content{quint64(bytes.size()), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
    auto exists = old_id.isNull() ? Result<dsfm::FileInfo>{StorageError{ErrorCode::MissingEntry}} : co_await manager_.stat(namespaceId(Private), old_id);
    if (!exists && exists.error().code != ErrorCode::MissingEntry) co_return exists.error();
    dsfm::CreateOptions options{root(Private), QStringLiteral("avatar-%1.png").arg(QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex())), dsfm::ConflictPolicy::Fail, content};
    auto created = exists ? co_await manager_.replaceFile(namespaceId(Private), old_id, content) : co_await manager_.createFile(namespaceId(Private), options);
    if (!created) co_return created.error();
    auto pending = std::move(created).value();
    QVariant owner_identity, owner_contact, owner_notification;
    const bool identity_asset = key.startsWith("identity/"), contact_asset = key.startsWith("contact/"), request_asset = key.startsWith("request/");
    const bool preview_asset = key.startsWith("preview/v1/");
    if (preview_asset) {
        const auto parts = key.split('/');
        bool valid_id = false;
        const auto file_id = parts.size() == 4 ? parts.at(2).toInt(&valid_id) : 0;
        if (!valid_id || file_id <= 0 || parts.at(3).size() != 64
            || bytes.size() > media_preview::max_encoded_bytes) {
            co_await pending.writer->abort(); co_return StorageError{ErrorCode::InvalidArgument};
        }
        query.prepare("SELECT identity_id,contact_id,hash FROM file WHERE id=:id");
        query.bindValue(":id", file_id);
        if (!query.exec() || !query.next()
            || parts.at(3).toLatin1() != query.value(2).toByteArray().toHex()) {
            co_await pending.writer->abort(); co_return StorageError{ErrorCode::Cancelled};
        }
        owner_identity = query.value(0); owner_contact = query.value(1);
    } else if (identity_asset || contact_asset || request_asset) {
        if (request_asset) {
            query.prepare("SELECT id,identity FROM notification WHERE hash=:key AND type=0");
            query.bindValue(":key", key.sliced(8));
        } else {
            query.prepare(identity_asset ? "SELECT id,id FROM identity WHERE uuid=:key" : "SELECT id,identity FROM contact WHERE uuid=:key");
            query.bindValue(":key", QUuid(key.sliced(identity_asset ? 9 : 8)));
        }
        if (!query.exec() || !query.next()) { co_await pending.writer->abort(); co_return StorageError{ErrorCode::Cancelled}; }
        owner_identity = query.value(1);
        if (contact_asset) owner_contact = query.value(0);
        if (request_asset) owner_notification = query.value(0);
    }
    query.prepare("INSERT INTO media_asset(asset_key,file_id,pending_id,deleted,owner_identity,owner_contact,owner_notification) VALUES(:key,:file,:pending,0,:identity,:contact,:notification) "
                  "ON CONFLICT(asset_key) DO UPDATE SET file_id=excluded.file_id,pending_id=excluded.pending_id,deleted=0,owner_identity=excluded.owner_identity,owner_contact=excluded.owner_contact,owner_notification=excluded.owner_notification");
    query.bindValue(":key", key);
    query.bindValue(":file", pending.info.file_id);
    query.bindValue(":pending", pending.info.id);
    query.bindValue(":identity", owner_identity); query.bindValue(":contact", owner_contact); query.bindValue(":notification", owner_notification);
    if (!query.exec()) { co_await pending.writer->abort(); co_return StorageError{ErrorCode::IoError}; }
    for (qsizetype offset = 0; offset < bytes.size(); offset += io_bytes) {
        const auto chunk = bytes.mid(offset, io_bytes);
        auto written = co_await pending.writer->writeAt(quint64(offset), chunk);
        if (!written || written.value() != chunk.size()) co_return written ? StorageError{ErrorCode::IoError} : written.error();
    }
    query.prepare("SELECT deleted FROM media_asset WHERE asset_key=:key"); query.bindValue(":key", key);
    if (!query.exec() || !query.next() || query.value(0).toBool()) { co_await pending.writer->abort(); co_return StorageError{ErrorCode::Cancelled}; }
    query.finish();
    auto committed = co_await pending.writer->commit();
    if (!committed) co_return committed.error();
    query.prepare("UPDATE media_asset SET pending_id=NULL WHERE asset_key=:key");
    query.bindValue(":key", key);
    if (!query.exec()) co_return StorageError{ErrorCode::IoError};
    co_return Result<void>{};
}
QString ManagedStorage::identityAvatar(QUuid id) { return "identity/" + uuid(id); }
QString ManagedStorage::contactAvatar(QUuid id) { return "contact/" + uuid(id); }
QString ManagedStorage::filePreview(int file_id, QByteArray sha256) {
    return QStringLiteral("preview/v1/%1/%2").arg(file_id).arg(QString::fromLatin1(sha256.toHex()));
}

QCoro::Task<Result<QByteArray>> ManagedStorage::imagePreview(int file_id) {
    if (file_id <= 0 || shutdown_requested_ || preview_calls_ >= 16)
        co_return StorageError{ErrorCode::QueueFull};
    auto operation = holdOperation();
    ++preview_calls_;
    struct Guard {
        ManagedStorage* self; bool owns = false;
        ~Guard() {
            if (owns) self->preview_busy_ = false;
            --self->preview_calls_; emit self->drained();
        }
    } guard{this};
    while (preview_busy_) co_await qCoro(this, &ManagedStorage::drained);
    if (shutdown_requested_) co_return StorageError{ErrorCode::Cancelled};
    preview_busy_ = true; guard.owns = true;
    auto initialized = co_await ready();
    if (!initialized) co_return initialized.error();
    QSqlQuery query;
    query.prepare("SELECT storage_namespace,storage_id,hash,size,direction,state,mime_type FROM file WHERE id=:id");
    query.bindValue(":id", file_id);
    if (!query.exec()) co_return StorageError{ErrorCode::IoError};
    if (!query.next()) co_return StorageError{ErrorCode::MissingEntry};
    auto ns = query.value(0).toUuid(), id = query.value(1).toUuid();
    const auto sha256 = query.value(2).toByteArray();
    auto source_sha256 = sha256;
    auto size = query.value(3).toLongLong();
    const auto incoming = query.value(4).toInt() == File::INCOMING;
    const auto state = query.value(5).toInt();
    const auto mime_type = query.value(6).toString();
    const auto video = media_preview::isVideoCandidate(mime_type);
    if (sha256.size() != 32 || (!mime_type.startsWith("image/") && !video))
        co_return StorageError{ErrorCode::InvalidArgument};
    query.finish();
    bool thumbnail = false;
    if (incoming) {
        query.prepare("SELECT f.storage_namespace,f.storage_id,f.hash,f.size,f.mime_type FROM file_thumbnail_v1 t JOIN file f ON f.id=t.file_id JOIN file p ON p.id=t.primary_id WHERE t.primary_id=? AND f.direction=1 AND f.state=6 AND f.contact_id=p.contact_id AND f.identity_id=p.identity_id AND f.conversation_id=p.conversation_id");
        query.addBindValue(file_id);
        if (!query.exec()) co_return StorageError{ErrorCode::IoError};
        if (query.next()) {
            ns = query.value(0).toUuid(); id = query.value(1).toUuid(); source_sha256 = query.value(2).toByteArray();
            size = query.value(3).toLongLong();
            if (query.value(4).toString() != "image/jpeg" || size > media_preview::max_encoded_bytes)
                co_return StorageError{ErrorCode::InvalidArgument};
            thumbnail = true;
        }
        query.finish();
    }
    if (ns.isNull() || id.isNull() || source_sha256.size() != 32
        || size <= 0 || size > (video && !thumbnail ? media_preview::max_video_source_bytes : media_preview::max_source_bytes)
        || (!thumbnail && incoming && state != File::FS_DONE))
        co_return StorageError{ErrorCode::InvalidArgument};
    const auto key = filePreview(file_id, sha256);
    auto cached = co_await readAsset(key);
    if (cached) {
        auto valid = co_await disk<bool>([bytes = cached.value()] {
            return Result<bool>{!media_preview::decode(bytes).isNull()};
        });
        if (valid && valid.value()) {
            LFLOG_DEBUG << "Using cached media preview";
            co_return cached.value();
        }
        auto removed = co_await writeAssetImpl(key, {}, true);
        if (!removed) co_return removed.error();
    } else if (cached.error().code == ErrorCode::IntegrityError) {
        auto removed = co_await writeAssetImpl(key, {}, true);
        if (!removed) co_return removed.error();
    } else if (cached.error().code != ErrorCode::MissingEntry) co_return cached.error();
    if (video && !thumbnail && media_preview::ffmpegExecutable().isEmpty())
        co_return StorageError{ErrorCode::InvalidArgument};
    auto info = co_await manager_.stat(ns, id);
    if (!info) co_return info.error();
    if (info.value().size != quint64(size) || info.value().sha256 != source_sha256)
        co_return StorageError{ErrorCode::IntegrityError};
    auto opened = co_await manager_.openRead(ns, id);
    if (!opened) co_return opened.error();
    auto reader = std::move(opened).value();
    QByteArray source;
    source.reserve(qsizetype(size));
    while (source.size() < size) {
        if (preview_stop_.stop_requested()) { co_await reader->close(); co_return StorageError{ErrorCode::Cancelled}; }
        const auto requested = qMin<qint64>(io_bytes, size - source.size());
        auto chunk = co_await reader->readAt(quint64(source.size()), requested);
        if (!chunk || chunk.value().isEmpty() || chunk.value().size() > requested) {
            co_await reader->close();
            co_return chunk ? StorageError{ErrorCode::IoError} : chunk.error();
        }
        source.append(chunk.value());
    }
    auto closed = co_await reader->close();
    if (!closed) co_return closed.error();
    auto generated = co_await disk<QByteArray>([source = std::move(source), source_sha256, thumbnail, incoming, mime_type, video, stop = preview_stop_.get_token()]() mutable {
        if (stop.stop_requested()) return Result<QByteArray>{StorageError{ErrorCode::Cancelled}};
        if (QCryptographicHash::hash(source, QCryptographicHash::Sha256) != source_sha256)
            return Result<QByteArray>{StorageError{ErrorCode::IntegrityError}};
        if (thumbnail && media_preview::decode(source).isNull())
            return Result<QByteArray>{StorageError{ErrorCode::InvalidArgument}};
        if (incoming && !thumbnail) {
            const QMimeDatabase database;
            if (database.mimeTypeForData(source).name() != database.mimeTypeForName(mime_type).name())
                return Result<QByteArray>{StorageError{ErrorCode::InvalidArgument}};
        }
        auto bytes = video && !thumbnail ? media_preview::generateVideo(std::move(source), stop) : media_preview::generate(std::move(source));
        return bytes.isEmpty() ? Result<QByteArray>{StorageError{ErrorCode::InvalidArgument}}
                               : Result<QByteArray>{std::move(bytes)};
    });
    if (!generated) co_return generated.error();
    // Generation is serialized, including eviction/publication. Even many
    // concurrent views cannot exceed the persistent preview count bound.
    if (!query.exec("SELECT asset_key FROM media_asset WHERE asset_key>='preview/v1/' AND asset_key<'preview/v10' ORDER BY rowid LIMIT 513"))
        co_return StorageError{ErrorCode::IoError};
    QStringList keys;
    while (query.next()) {
        if (keys.size() >= media_preview::max_cached_previews) co_return StorageError{ErrorCode::QuotaExceeded};
        keys.push_back(query.value(0).toString());
    }
    query.finish();
    if (keys.size() >= media_preview::max_cached_previews) {
        auto removed = co_await writeAssetImpl(keys.front(), {}, true);
        if (!removed) co_return removed.error();
    }
    auto saved = co_await writeAssetImpl(key, generated.value(), true);
    if (!saved) co_return saved.error();
    LFLOG_DEBUG << "Generated and cached media preview; version=" << media_preview::version;
    co_return generated.value();
}
void ManagedStorage::saveAvatar(QString key, QImage image) {
    if (shutdown_requested_ || stopping_ || (avatar_queue_.size() >= 64 && !avatar_queue_.contains(key))) { emit storageError(); return; }
    QByteArray bytes;
    image = normalizeAvatar(image);
    if (!image.isNull()) {
        QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG") || bytes.size() > avatar_bytes) { emit storageError(); return; }
    }
    const bool queued = avatar_queue_.contains(key);
    avatar_queue_.insert(key, std::move(bytes));
    if (!queued) saveAvatars(key);
}
QCoro::Task<void> ManagedStorage::saveAvatars(QString key) {
    // The in-flight value remains in the map, so later edits coalesce instead of starting another writer.
    while (avatar_queue_.contains(key)) {
        const auto bytes = avatar_queue_.value(key);
        auto result = co_await writeAsset(key, bytes);
        if (!result) { LFLOG_WARN << "Saving managed avatar failed (code=" << int(result.error().code) << ")"; emit storageError(); }
        if (avatar_queue_.value(key) == bytes) avatar_queue_.remove(key);
    }
    emit drained();
}
void ManagedStorage::loadAvatar(QString key, QObject* context, std::function<void(QImage)> apply) {
    restoreAvatar(std::move(key), QPointer<QObject>(context), std::move(apply));
}
QCoro::Task<void> ManagedStorage::restoreAvatar(QString key, QPointer<QObject> context, std::function<void(QImage)> apply) {
    auto bytes = co_await readAsset(key);
    if (!context || !bytes) co_return;
    QBuffer buffer(&bytes.value()); buffer.open(QIODevice::ReadOnly);
    QImageReader decoder(&buffer, "PNG");
    const auto size = decoder.size();
    if (size.width() <= 0 || size.height() <= 0 || size.width() > MaxAvatarDimension || size.height() > MaxAvatarDimension) co_return;
    const auto image = decoder.read();
    if (!image.isNull()) apply(image);
}
void ManagedStorage::removeAsset(QString key) { eraseAsset(std::move(key)); }
QCoro::Task<void> ManagedStorage::eraseAsset(QString key) {
    auto removed = co_await writeAsset(std::move(key), {});
    if (!removed) { LFLOG_WARN << "Removing managed avatar failed (code=" << int(removed.error().code) << ")"; emit storageError(); }
}
QCoro::Task<Result<void>> ManagedStorage::collectGarbage() {
    if (collecting_) co_return Result<void>{};
    if (!ready_) {
        auto initialized = co_await ready();
        if (!initialized) co_return initialized.error();
    }
    collecting_ = true;
    ++active_;
    struct Guard { ManagedStorage* self; ~Guard() { self->collecting_ = false; --self->active_; emit self->drained(); } } guard{this};
    struct Item { QUuid ns, id, pending; };
    QVector<Item> items;
    QSqlQuery query;
    if (!query.exec("SELECT storage_namespace,storage_id,pending_id FROM storage_gc LIMIT 32")) co_return StorageError{ErrorCode::IoError};
    while (query.next()) items.push_back({query.value(0).toUuid(),query.value(1).toUuid(),query.value(2).toUuid()});
    query.finish();
    for (const auto item : items) {
        if (!isDownloadNamespace(item.ns) && item.ns != namespaceId(Private)) continue;
        if (!item.pending.isNull()) {
            auto aborted = co_await manager_.abortPending(item.ns, item.pending);
            if (!aborted && aborted.error().code != ErrorCode::MissingEntry) continue;
        }
        auto removed = co_await manager_.remove(item.ns, item.id);
        if (!removed && removed.error().code != ErrorCode::MissingEntry) continue;
        query.prepare("DELETE FROM storage_gc WHERE storage_namespace=:ns AND storage_id=:id");
        query.bindValue(":ns", item.ns); query.bindValue(":id", item.id);
        if (!query.exec()) co_return StorageError{ErrorCode::IoError};
    }
    QStringList deleted;
    if (!query.exec("SELECT asset_key FROM media_asset WHERE deleted=1 LIMIT 32")) co_return StorageError{ErrorCode::IoError};
    while (query.next()) deleted.push_back(query.value(0).toString());
    query.finish();
    for (const auto& key : deleted) {
        auto removed = co_await writeAsset(key, {});
        if (!removed && removed.error().code != ErrorCode::Busy) LFLOG_WARN << "Managed media cleanup deferred (code=" << int(removed.error().code) << ")";
    }
    // Bridge the two independent catalogs: reclaim abandoned reservations that never acquired
    // an application reference. An active writer is protected by the library's Busy response.
    auto spaces = co_await manager_.namespaces();
    if (!spaces) co_return spaces.error();
    quint32 orphan_budget = 32;
    for (const auto& space : spaces.value()) {
        if (!orphan_budget) break;
        auto pending = co_await manager_.listPending(space.config.id);
        if (!pending) continue;
        for (const auto& item : pending.value().entries) {
            if (!orphan_budget) break;
            query.prepare("SELECT 1 FROM file WHERE storage_namespace=:ns AND pending_id=:pending "
                          "UNION ALL SELECT 1 FROM media_asset WHERE pending_id=:pending AND :ns=:private LIMIT 1");
            query.bindValue(":ns", space.config.id); query.bindValue(":private", namespaceId(Private)); query.bindValue(":pending", item.id);
            if (!query.exec()) co_return StorageError{ErrorCode::IoError};
            const bool referenced = query.next(); query.finish();
            if (!referenced) { --orphan_budget; co_await manager_.abortPending(space.config.id, item.id); }
        }
    }
    co_return Result<void>{};
}
QCoro::Task<Result<void>> ManagedStorage::shutdown() {
    if (shutdown_requested_) {
        while (!closed_) co_await qCoro(this, &ManagedStorage::drained);
        co_return shutdown_error_ ? Result<void>{*shutdown_error_} : Result<void>{};
    }
    shutdown_requested_ = true;
    preview_stop_.request_stop();
    while (active_ || !avatar_queue_.isEmpty() || initializing_) co_await qCoro(this, &ManagedStorage::drained);
    if (ready_) co_await collectGarbage();
    stopping_ = true;
    auto stopped = co_await manager_.shutdown();
    if (!stopped) shutdown_error_ = stopped.error();
    closed_ = true;
    emit drained();
    co_return stopped;
}
} // namespace ds::core
