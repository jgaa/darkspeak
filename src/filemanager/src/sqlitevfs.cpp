// Descriptor-relative SQLite VFS. The owning process lock excludes other
// connections/processes; namespace scheduling serializes this catalog's users.
#include "internal.h"
#include <sqlite3.h>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

namespace dsfm::detail {
namespace {
struct SqlFile { sqlite3_file base; int fd; int dir; quint64 dev; quint64 ino; char name[32]; };
SqlFile* file(sqlite3_file* p) { return reinterpret_cast<SqlFile*>(p); }
int errorCode(int fallback) { return errno == ENOSPC || errno == EDQUOT ? SQLITE_FULL : fallback; }
int closeFile(sqlite3_file* p) { const auto result = ::close(file(p)->fd); file(p)->fd = -1; return result == 0 ? SQLITE_OK : SQLITE_IOERR_CLOSE; }
int readFile(sqlite3_file* p, void* bytes, int amount, sqlite3_int64 offset) {
    auto* target = static_cast<char*>(bytes); int done = 0;
    if (amount < 0 || offset < 0 || offset > std::numeric_limits<qint64>::max() - amount) return SQLITE_IOERR_READ;
    while (done < amount) {
        const auto count = ::pread(file(p)->fd, target + done, size_t(amount - done), off_t(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return errorCode(SQLITE_IOERR_READ);
        if (!count) { std::memset(target + done, 0, size_t(amount - done)); return SQLITE_IOERR_SHORT_READ; }
        done += int(count);
    }
    return SQLITE_OK;
}
int writeFile(sqlite3_file* p, const void* bytes, int amount, sqlite3_int64 offset) {
    const auto* source = static_cast<const char*>(bytes); int done = 0;
    if (amount < 0 || offset < 0 || offset > std::numeric_limits<qint64>::max() - amount) return SQLITE_IOERR_WRITE;
    while (done < amount) {
        const auto count = ::pwrite(file(p)->fd, source + done, size_t(amount - done), off_t(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return errorCode(SQLITE_IOERR_WRITE);
        done += int(count);
    }
    return SQLITE_OK;
}
int truncateFile(sqlite3_file* p, sqlite3_int64 size) { return size >= 0 && ::ftruncate(file(p)->fd, size) == 0 ? SQLITE_OK : errorCode(SQLITE_IOERR_TRUNCATE); }
int syncFile(sqlite3_file* p, int) {
    if (::fsync(file(p)->fd) != 0) return errorCode(SQLITE_IOERR_FSYNC);
    return ::fsync(file(p)->dir) == 0 ? SQLITE_OK : SQLITE_IOERR_DIR_FSYNC;
}
int sizeFile(sqlite3_file* p, sqlite3_int64* size) { struct stat st{}; if (::fstat(file(p)->fd, &st)) return SQLITE_IOERR_FSTAT; *size = st.st_size; return SQLITE_OK; }
int lockFile(sqlite3_file*, int) { return SQLITE_OK; }
int reservedFile(sqlite3_file*, int* reserved) { *reserved = 0; return SQLITE_OK; }
int controlFile(sqlite3_file* p, int op, void* arg) {
    if (op == SQLITE_FCNTL_HAS_MOVED) {
        struct stat st{}; *static_cast<int*>(arg) = ::fstatat(file(p)->dir, file(p)->name, &st, AT_SYMLINK_NOFOLLOW) != 0
            || quint64(st.st_dev) != file(p)->dev || quint64(st.st_ino) != file(p)->ino;
        return SQLITE_OK;
    }
    if (op == SQLITE_FCNTL_VFSNAME) { *static_cast<char**>(arg) = sqlite3_mprintf("dsfm"); return SQLITE_OK; }
    return SQLITE_NOTFOUND;
}
int sectorFile(sqlite3_file*) { return 4096; }
int characteristicsFile(sqlite3_file*) { return 0; }
const sqlite3_io_methods methods{1, closeFile, readFile, writeFile, truncateFile, syncFile, sizeFile, lockFile, lockFile,
    reservedFile, controlFile, sectorFile, characteristicsFile, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
std::pair<int, QString> parse(const char* path) {
    require(path && std::strlen(path) <= 256, ErrorCode::InvalidArgument);
    const auto parts = QString::fromUtf8(path).split('/');
    require(parts.size() == 4 && parts[0].isEmpty() && parts[1] == "dsfm", ErrorCode::InvalidArgument);
    bool ok = false; const auto fd = parts[2].toInt(&ok);
    require(ok && fd >= 0 && (parts[3] == "catalog.db" || parts[3] == "catalog.db-journal" || parts[3] == "catalog.db-wal" || parts[3] == "catalog.db-shm"), ErrorCode::InvalidArgument);
    return {fd, parts[3]};
}
int openFile(sqlite3_vfs*, const char* path, sqlite3_file* p, int flags, int* out_flags) {
    p->pMethods = nullptr;
    try {
        auto [dir, name] = parse(path);
        int native_flags = (flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR;
        if (flags & SQLITE_OPEN_CREATE) native_flags |= O_CREAT;
        if (flags & SQLITE_OPEN_EXCLUSIVE) native_flags |= O_EXCL;
        auto fd = openBeneath(dir, name, native_flags | O_NONBLOCK, (native_flags & O_CREAT) ? 0600 : 0);
        const auto id = identity(fd.value);
        auto* target = file(p); target->fd = std::exchange(fd.value, -1); target->dir = dir; target->dev = id.dev; target->ino = id.ino;
        std::strcpy(target->name, name.toLatin1().constData());
        p->pMethods = &methods; if (out_flags) *out_flags = flags;
        return SQLITE_OK;
    } catch (...) { return errorCode(SQLITE_CANTOPEN); }
}
int deleteFile(sqlite3_vfs*, const char* path, int sync_dir) {
    try {
        auto [dir, name] = parse(path);
        auto fd = openBeneath(dir, name, O_RDONLY | O_NONBLOCK); identity(fd.value);
        if (::unlinkat(dir, name.toLatin1().constData(), 0) < 0) return SQLITE_IOERR_DELETE;
        if (sync_dir && ::fsync(dir) < 0) return SQLITE_IOERR_DIR_FSYNC;
        return SQLITE_OK;
    } catch (const Failure& failure) { return failure.error.code == ErrorCode::MissingEntry ? SQLITE_IOERR_DELETE_NOENT : SQLITE_IOERR_DELETE; }
}
int accessFile(sqlite3_vfs*, const char* path, int, int* present) {
    try { auto [dir, name] = parse(path); auto fd = openBeneath(dir, name, O_RDONLY | O_NONBLOCK); identity(fd.value); *present = 1; return SQLITE_OK; }
    catch (const Failure& failure) { *present = 0; return failure.error.code == ErrorCode::MissingEntry ? SQLITE_OK : SQLITE_IOERR_ACCESS; }
}
int fullPath(sqlite3_vfs*, const char* path, int count, char* output) {
    try { parse(path); if (int(std::strlen(path)) >= count) return SQLITE_CANTOPEN; std::strcpy(output, path); return SQLITE_OK; }
    catch (...) { return SQLITE_CANTOPEN; }
}
sqlite3_vfs vfs{};
} // namespace
void registerSqliteVfs() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto* original = sqlite3_vfs_find(nullptr); require(original != nullptr, ErrorCode::UnsupportedCapability);
        vfs = *original;
        vfs.iVersion = 1; vfs.szOsFile = sizeof(SqlFile); vfs.mxPathname = 256; vfs.zName = "dsfm"; vfs.pNext = nullptr;
        vfs.xOpen = openFile; vfs.xDelete = deleteFile; vfs.xAccess = accessFile; vfs.xFullPathname = fullPath;
        // Other callbacks belong to the system VFS and receive its own state.
        vfs.pAppData = original->pAppData;
        require(sqlite3_vfs_register(&vfs, 0) == SQLITE_OK, ErrorCode::UnsupportedCapability);
    });
}
} // namespace dsfm::detail
