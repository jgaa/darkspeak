// Test-only FFmpeg replacement. A successful sandbox probe echoes a supplied
// tiny JPEG; no application credentials or real user files are used.
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    if (::getenv("DARKSPEAK_VIDEO_PROBE_SECRET")) return 10;
    if (::open("/etc/passwd", O_RDONLY) >= 0 || errno != EACCES) return 11;
    if (::open("/tmp", O_RDONLY | O_DIRECTORY) >= 0 || errno != EACCES) return 25;
    if (::socket(AF_INET, SOCK_STREAM, 0) >= 0 || errno != EPERM) return 12;
    if (::fork() >= 0 || errno != EPERM) return 13;
    if (!(::fcntl(STDIN_FILENO, F_GET_SEALS) & F_SEAL_WRITE)) return 14;
    ::signal(SIGXFSZ, SIG_IGN);
    if (::pwrite(STDIN_FILENO, "x", 1, 0) >= 0 || (errno != EPERM && errno != EFBIG)) return 14;
    for (int fd = 3; fd < 64; ++fd) if (::fcntl(fd, F_GETFD) >= 0) return 15;
    rlimit limit{};
    if (::getrlimit(RLIMIT_CORE, &limit) || limit.rlim_cur != 0) return 16;
    if (::getrlimit(RLIMIT_CPU, &limit) || limit.rlim_cur > 5) return 17;
    if (::getrlimit(RLIMIT_AS, &limit) || limit.rlim_cur > 768ULL * 1024 * 1024) return 18;
    if (::getrlimit(RLIMIT_FSIZE, &limit) || limit.rlim_cur != 0) return 19;
    if (::lseek(STDIN_FILENO, 0, SEEK_SET) < 0) return 20;
    std::array<char, 65536> input{};
    const auto count = ::read(STDIN_FILENO, input.data(), input.size());
    if (count < 8) return 21;
    const auto* newline = static_cast<const char*>(std::memchr(input.data(), '\n', size_t(count)));
    if (!newline) return 22;
    if (std::memcmp(input.data(), "wait\n", 5) == 0) ::sleep(30);
    if (std::memcmp(input.data(), "flood\n", 6) == 0) {
        for (int i = 0; i < 4; ++i) if (::write(STDOUT_FILENO, input.data(), input.size()) < 0) return 23;
        return 0;
    }
    const auto length = count - (newline + 1 - input.data());
    return ::write(STDOUT_FILENO, newline + 1, size_t(length)) == length ? 0 : 24;
}
