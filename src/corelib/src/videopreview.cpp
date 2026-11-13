#include "ds/mediapreview.h"
#include "ds/ffmpeg.h"
#include "logfault/logfault.h"
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/memfd.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
// Only async-signal-safe operations below: QProcess calls this after fork.
bool restrictFiles(const char* executable) {
    const auto abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return false; // Fail closed when the kernel cannot isolate the decoder.
    landlock_ruleset_attr rules{};
    rules.handled_access_fs = (LANDLOCK_ACCESS_FS_MAKE_SYM << 1) - 1;
    // Stable UAPI bits: allow building with ABI-1 headers (Linux 5.13+) while
    // negotiating newer protections with the running kernel.
    if (abi >= 2) rules.handled_access_fs |= 1ULL << 13; // LANDLOCK_ACCESS_FS_REFER
    if (abi >= 3) rules.handled_access_fs |= 1ULL << 14; // LANDLOCK_ACCESS_FS_TRUNCATE
    const int rules_fd = int(::syscall(SYS_landlock_create_ruleset, &rules, sizeof(rules), 0));
    if (rules_fd < 0) return false;
    bool success = true;
    // Runtime libraries/loader and the chosen executable; no home, download or
    // temporary-directory hierarchy is granted. Input is already open.
    const char* paths[]{"/usr/lib", "/usr/lib64", "/usr/local/lib", "/lib", "/lib64",
                        "/app/lib", "/etc/ld.so.cache", "/dev/urandom", executable};
    for (const auto* path : paths) {
        const int fd = ::open(path, O_PATH | O_CLOEXEC);
        if (fd < 0) { if (errno != ENOENT) success = false; continue; }
        struct stat info{};
        if (::fstat(fd, &info)) success = false;
        landlock_path_beneath_attr allowed{};
        allowed.parent_fd = fd;
        allowed.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE;
        if (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) allowed.allowed_access |= LANDLOCK_ACCESS_FS_EXECUTE;
        if (S_ISDIR(info.st_mode)) allowed.allowed_access |= LANDLOCK_ACCESS_FS_READ_DIR;
        if (::syscall(SYS_landlock_add_rule, rules_fd, LANDLOCK_RULE_PATH_BENEATH, &allowed, 0)) success = false;
        ::close(fd);
    }
    // Anonymous memfd objects are outside filesystem hierarchies and cannot
    // receive a Landlock path rule. The sealed, preopened input remains usable.
    if (success && ::syscall(SYS_landlock_restrict_self, rules_fd, 0)) success = false;
    ::close(rules_fd);
    return success;
}

bool restrictSyscalls() {
#if defined(__x86_64__)
    constexpr auto architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr auto architecture = AUDIT_ARCH_AARCH64;
#else
    return false; // Audit the syscall ABI before enabling another Linux architecture.
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    const sock_filter filter[]{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        // Reject alternate syscall ABIs (including x32).
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#define DENY_VIDEO_SYSCALL(name) \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_##name, 0, 1), \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
        DENY_VIDEO_SYSCALL(socket), DENY_VIDEO_SYSCALL(socketpair), DENY_VIDEO_SYSCALL(connect),
        DENY_VIDEO_SYSCALL(ptrace), DENY_VIDEO_SYSCALL(process_vm_readv), DENY_VIDEO_SYSCALL(process_vm_writev),
        DENY_VIDEO_SYSCALL(pidfd_getfd), DENY_VIDEO_SYSCALL(io_uring_setup),
        DENY_VIDEO_SYSCALL(truncate), DENY_VIDEO_SYSCALL(ftruncate),
#ifdef SYS_fork
        DENY_VIDEO_SYSCALL(fork),
#endif
#ifdef SYS_vfork
        DENY_VIDEO_SYSCALL(vfork),
#endif
#undef DENY_VIDEO_SYSCALL
        // Allow decoder threads, but no child processes. Let libc fall back
        // from clone3 to clone, whose flags can be inspected by seccomp.
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x00010000u /* CLONE_THREAD */, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    const sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), const_cast<sock_filter*>(filter)};
    return !::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
#endif
}
}
#endif

namespace ds::core::media_preview {
QByteArray generateVideo(QByteArray source, std::stop_token stop) {
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (stop.stop_requested() || source.isEmpty() || source.size() > max_video_source_bytes) return {};
    const auto executable = ffmpegExecutable();
    if (executable.isEmpty()) return {};
    const int created_fd = int(::syscall(SYS_memfd_create, "darkspeak-video", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (created_fd < 0) return {};
    // A GUI launcher may leave stdin closed. Keep the source above stdio so
    // QProcess cannot overwrite it while installing the child's channels.
    const int fd = ::fcntl(created_fd, F_DUPFD_CLOEXEC, 3);
    ::close(created_fd);
    if (fd < 0) return {};
    auto close_input = qScopeGuard([fd] { ::close(fd); });
    if (::ftruncate(fd, 0) || ::lseek(fd, 0, SEEK_SET) < 0) return {};
    for (qsizetype offset = 0; offset < source.size();) {
        if (stop.stop_requested()) return {};
        const auto count = ::write(fd, source.constData() + offset, size_t(source.size() - offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return {};
        offset += count;
    }
    if (::lseek(fd, 0, SEEK_SET) < 0
        || ::fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) return {};
    source.clear();
    QProcess process;
    auto reap = qScopeGuard([&process] {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); }
    });
    QProcessEnvironment environment;
    environment.insert("LC_ALL", "C"); environment.insert("HOME", "/nonexistent");
    environment.insert("OMP_NUM_THREADS", "1"); environment.insert("OPENBLAS_NUM_THREADS", "1");
    process.setProcessEnvironment(environment); // Never inherit credentials or profile environment.
    process.setWorkingDirectory("/");
    process.setStandardErrorFile(QProcess::nullDevice()); // Decoder diagnostics may contain private metadata.
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors
        | QProcess::UnixProcessFlag::ResetIds | QProcess::UnixProcessFlag::DisableCoreDumps);
    const auto program = QFile::encodeName(executable);
    const auto parent_pid = ::getpid();
    process.setChildProcessModifier([fd, program, parent_pid, &process] {
        const rlimit memory{768ULL * 1024 * 1024, 768ULL * 1024 * 1024};
        const rlimit cpu{5, 5}, files{0, 0}, descriptors{64, 64};
        if (::dup2(fd, STDIN_FILENO) < 0 || ::setrlimit(RLIMIT_AS, &memory)
            || ::setrlimit(RLIMIT_CPU, &cpu) || ::setrlimit(RLIMIT_FSIZE, &files)
            || ::setrlimit(RLIMIT_NOFILE, &descriptors))
            process.failChildProcessModifier("video preview resource limits", errno);
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) || ::getppid() != parent_pid
            || ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
            process.failChildProcessModifier("video preview privilege boundary", errno);
        if (!restrictFiles(program.constData()))
            process.failChildProcessModifier("video preview filesystem isolation", errno);
        if (!restrictSyscalls()) process.failChildProcessModifier("video preview syscall isolation", errno);
    });
    QElapsedTimer deadline; deadline.start();
    process.start(executable, {"-hide_banner", "-loglevel", "error", "-nostdin", "-max_alloc", "67108864",
        "-threads", "1", "-filter_threads", "1", "-hwaccel", "none", "-protocol_whitelist", "file",
        "-format_whitelist", "mov,matroska,webm,avi,ogg,mpegts,mpegvideo,yuv4mpegpipe",
        // A seekable descriptor alias also works on distribution FFmpeg versions
        // that predate its fd: protocol, without opening profile paths.
        "-probesize", "1048576", "-analyzeduration", "1000000", "-i", "file:/proc/self/fd/0",
        "-map", "0:v:0", "-frames:v", "1", "-an", "-sn", "-dn", "-map_metadata", "-1", "-map_chapters", "-1",
        // Convert display aspect (including non-square source pixels) to a
        // small square-pixel image before dropping all metadata.
        "-vf", "scale=w='max(1,if(gte(dar,1),min(320,iw*sar),min(320,ih)*dar))':h='max(1,if(gte(dar,1),min(320,iw*sar)/dar,min(320,ih)))',setsar=1",
        "-threads", "1", "-c:v", "mjpeg", "-q:v", "5", "-f", "image2pipe", "-protocol_whitelist", "pipe", "pipe:1"});
    if (!process.waitForStarted(1000)) {
        const auto error = process.errorString();
        const int stage = error.contains("resource limits") ? 1 : error.contains("privilege boundary") ? 2
            : error.contains("filesystem isolation") ? 3 : error.contains("syscall isolation") ? 4 : 0;
        LFLOG_DEBUG << "Video preview helper could not start (code=" << int(process.error()) << ", stage=" << stage << ")";
        return {};
    }
    QByteArray output;
    do {
        process.waitForReadyRead(50);
        if (stop.stop_requested() || deadline.elapsed() >= video_deadline_ms || process.bytesAvailable() > max_encoded_bytes - output.size()) {
            LFLOG_DEBUG << "Video preview helper exceeded time or output limits";
            return {};
        }
        output += process.readAllStandardOutput();
    } while (process.state() != QProcess::NotRunning);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || decode(output).isNull()) {
        LFLOG_DEBUG << "Video preview helper failed or returned an unsupported frame (exit=" << process.exitCode()
                    << ", status=" << int(process.exitStatus()) << ", bytes=" << output.size() << ")";
        return {};
    }
    return generate(output); // Fresh canonical JPEG; no decoder metadata is retained.
#else
    // Find supported Windows, Android and macOS alternatives before enabling
    // those builds. Never run an unsandboxed Linux-command fallback there.
    Q_UNUSED(source);
    Q_UNUSED(stop);
    return {};
#endif
}
}
