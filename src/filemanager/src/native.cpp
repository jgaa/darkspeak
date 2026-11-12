#include "internal.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#ifdef __linux__
#include <linux/openat2.h>
#include <linux/fs.h>
#endif

namespace dsfm::detail {
void ioCheck(bool condition) {
    if (condition) return;
    auto code = ErrorCode::IoError;
    if (errno == ENOSPC || errno == EDQUOT) code = ErrorCode::DiskFull;
    else if (errno == EACCES || errno == EPERM) code = ErrorCode::PermissionDenied;
    else if (errno == ENOENT) code = ErrorCode::MissingEntry;
    else if (errno == EEXIST) code = ErrorCode::Conflict;
    else if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EXDEV) code = ErrorCode::UnsupportedCapability;
    else if (errno == ELOOP) code = ErrorCode::ExternalModification;
    fail(code, errno);
}
QString uuid(QUuid id) { return id.toString(QUuid::WithoutBraces); }
QUuid uuid(const QVariant& value) { return QUuid(value.toString()); }
Identity identity(int fd, bool directory) {
    struct stat info{};
    ioCheck(::fstat(fd, &info) == 0);
    require(directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode), ErrorCode::ExternalModification);
    require(info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0, ErrorCode::PermissionDenied);
    require(directory || info.st_nlink == 1, ErrorCode::ExternalModification);
    return {quint64(info.st_dev), quint64(info.st_ino), quint64(info.st_size),
            qint64(info.st_mtim.tv_sec) * 1000000000LL + info.st_mtim.tv_nsec, directory};
}
Fd openBeneath(int dir, const QString& path, int flags, int mode) {
#ifdef __linux__
    require(!path.isEmpty() && !path.startsWith('/') && path.size() <= 32768, ErrorCode::InvalidArgument);
    const auto components = path.split('/');
    for (const auto& component : components) require(!component.isEmpty() && component != "..", ErrorCode::InvalidArgument);
    struct open_how how{};
    how.flags = quint64(flags | O_CLOEXEC | O_NOFOLLOW);
    how.mode = quint64(mode);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    return Fd(int(::syscall(SYS_openat2, dir, QFile::encodeName(path).constData(), &how, sizeof(how))));
#else
    Q_UNUSED(dir); Q_UNUSED(path); Q_UNUSED(flags); Q_UNUSED(mode);
    fail(ErrorCode::UnsupportedCapability);
#endif
}
QString parentPath(QString path) {
    const auto slash = path.lastIndexOf('/');
    return slash < 0 ? QStringLiteral(".") : path.left(slash);
}
static QString basename(const QString& path) { return path.section('/', -1); }
void makeDirectory(int root, const QString& path) {
    auto parent = openBeneath(root, parentPath(path), O_RDONLY | O_DIRECTORY);
    ioCheck(::mkdirat(parent.value, QFile::encodeName(basename(path)).constData(), 0700) == 0);
    auto created = openBeneath(root, path, O_RDONLY | O_DIRECTORY);
    identity(created.value, true);
    ioCheck(::fsync(parent.value) == 0);
}
void renameSafe(int root, const QString& from, const QString& to, bool replace) {
#ifdef __linux__
    auto source = openBeneath(root, parentPath(from), O_RDONLY | O_DIRECTORY);
    auto destination = openBeneath(root, parentPath(to), O_RDONLY | O_DIRECTORY);
    ioCheck(::syscall(SYS_renameat2, source.value, QFile::encodeName(basename(from)).constData(),
                      destination.value, QFile::encodeName(basename(to)).constData(), replace ? 0 : RENAME_NOREPLACE) == 0);
#else
    Q_UNUSED(root); Q_UNUSED(from); Q_UNUSED(to); Q_UNUSED(replace); fail(ErrorCode::UnsupportedCapability);
#endif
}
void unlinkSafe(int root, const QString& path, bool directory) {
    auto parent = openBeneath(root, parentPath(path), O_RDONLY | O_DIRECTORY);
    ioCheck(::unlinkat(parent.value, QFile::encodeName(basename(path)).constData(), directory ? AT_REMOVEDIR : 0) == 0);
    ioCheck(::fsync(parent.value) == 0);
}
void syncDirectory(int root, const QString& path) {
    auto dir = openBeneath(root, path, O_RDONLY | O_DIRECTORY);
    ioCheck(::fsync(dir.value) == 0);
}
Fd openRoot(const QString& path, bool create, bool private_root) {
#ifdef __linux__
    require(path.startsWith('/') && path.size() <= 32768 && !path.contains(QChar::Null), ErrorCode::InvalidArgument);
    const auto clean = QDir::cleanPath(path);
    require(clean != "/" && clean == path, ErrorCode::InvalidArgument);
    Fd slash(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    struct open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
    const auto relative = clean.mid(1);
    auto fd = int(::syscall(SYS_openat2, slash.value, QFile::encodeName(relative).constData(), &how, sizeof(how)));
    if (fd < 0 && errno == ENOENT && create) {
        const auto parent_path = parentPath(relative);
        Fd parent(int(::syscall(SYS_openat2, slash.value, QFile::encodeName(parent_path).constData(), &how, sizeof(how))));
        ioCheck(::mkdirat(parent.value, QFile::encodeName(basename(relative)).constData(), 0700) == 0);
        fd = int(::syscall(SYS_openat2, slash.value, QFile::encodeName(relative).constData(), &how, sizeof(how)));
        ioCheck(::fsync(parent.value) == 0);
    }
    Fd result(fd);
    if (private_root) identity(result.value, true);
    struct statfs filesystem{}; ioCheck(::fstatfs(result.value, &filesystem) == 0);
    // Local filesystems with the required rename/locking/sync model. Network
    // filesystems cannot share the local ancestor-lock registry across hosts.
    const auto type = quint64(filesystem.f_type);
    require(type == 0xEF53 || type == 0x58465342 || type == 0x9123683E || type == 0x01021994 || type == 0x794c7630, ErrorCode::UnsupportedCapability);
    return result;
#else
    Q_UNUSED(path); Q_UNUSED(create); fail(ErrorCode::UnsupportedCapability);
#endif
}
QVector<std::shared_ptr<Fd>> lockRoots(int root, bool exclusive) {
    // Shared locks on ancestor filesystem identities, exclusive on the target.
    // Distinct open descriptions make flock enforce ownership within a process too.
    const auto lock_path = QStringLiteral("/tmp/dsfm-locks-%1").arg(::geteuid());
    if (::mkdir(QFile::encodeName(lock_path).constData(), 0700) < 0) ioCheck(errno == EEXIST);
    Fd registry(::open(QFile::encodeName(lock_path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    identity(registry.value, true);
    QVector<std::shared_ptr<Fd>> result;
    Fd current(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    bool target = true;
    for (quint32 depth = 0; depth < 512; ++depth) {
        struct stat st{};
        ioCheck(::fstat(current.value, &st) == 0);
        if (!target || !exclusive) {
            struct stat catalog{};
            if (::fstatat(current.value, ".filemanager", &catalog, AT_SYMLINK_NOFOLLOW) == 0)
                fail(ErrorCode::Conflict); // An existing managed ancestor cannot contain another namespace.
        }
        const auto key = QCryptographicHash::hash(QByteArray::number(st.st_dev) + ':' + QByteArray::number(st.st_ino), QCryptographicHash::Sha256).toHex();
        auto lock = std::make_shared<Fd>(::openat(registry.value, key.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        identity(lock->value);
        if (::flock(lock->value, (target && exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) < 0) fail(ErrorCode::NamespaceBusy, errno);
        result.append(lock);
        Fd parent(::openat(current.value, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        struct stat parent_st{};
        ioCheck(::fstat(parent.value, &parent_st) == 0);
        if (st.st_dev == parent_st.st_dev && st.st_ino == parent_st.st_ino) return result;
        current = std::move(parent);
        target = false;
    }
    fail(ErrorCode::UnsupportedCapability);
}
void cancelCheck(const CancellationToken& cancel) { if (cancel.cancelled()) fail(ErrorCode::Cancelled); }
void fault(const char* point) {
#ifdef DSFM_FAULT_INJECTION
    // Test-only process termination; production builds do not read this variable.
    if (qgetenv("DSFM_CRASH_AT") == point) ::_exit(86);
    if (qgetenv("DSFM_FAIL_AT") == point) fail(ErrorCode::DiskFull, ENOSPC);
    if (QByteArray(point) == "before_filesystem" && !qgetenv("DSFM_COLLISION_PATH").isEmpty()) {
        Fd collision(::open(qgetenv("DSFM_COLLISION_PATH").constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        ioCheck(::write(collision.value, "foreign", 7) == 7);
    }
#else
    Q_UNUSED(point);
#endif
}
QString pendingPath(PendingId id) { return QStringLiteral(".filemanager/staging/") + uuid(id); }
QVector<ByteRange> addRange(QVector<ByteRange> ranges, ByteRange range) {
    if (!range.length) return ranges;
    require(range.length <= quint64(std::numeric_limits<qint64>::max()) - range.offset, ErrorCode::InvalidArgument);
    ranges.append(range);
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.offset < b.offset; });
    QVector<ByteRange> result;
    for (auto next : ranges) {
        if (!result.isEmpty() && result.last().offset + result.last().length >= next.offset) {
            auto& back = result.last();
            back.length = std::max(back.offset + back.length, next.offset + next.length) - back.offset;
        } else result.append(next);
    }
    return result;
}
QVector<ByteRange> subtractRange(const QVector<ByteRange>& ranges, ByteRange range) {
    QVector<ByteRange> result;
    const auto end = range.offset + range.length;
    for (auto item : ranges) {
        const auto item_end = item.offset + item.length;
        if (item_end <= range.offset || item.offset >= end) result.append(item);
        else {
            if (item.offset < range.offset) result.append({item.offset, range.offset - item.offset});
            if (item_end > end) result.append({end, item_end - end});
        }
    }
    return result;
}
bool covers(const QVector<ByteRange>& ranges, ByteRange range) {
    if (!range.length) return true;
    for (auto item : ranges) if (item.offset <= range.offset && item.offset + item.length >= range.offset + range.length) return true;
    return false;
}
} // namespace dsfm::detail
