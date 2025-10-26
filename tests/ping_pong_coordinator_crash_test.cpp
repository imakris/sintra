#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <process.h>
#endif

namespace {

constexpr std::string_view kSharedDirEnv = "SINTRA_TEST_SHARED_DIR";
constexpr int kCrashAfterRounds = 5;
constexpr char kPingByte = 'P';
constexpr char kPongByte = 'Q';

std::filesystem::path shared_directory()
{
    const char* value = std::getenv(kSharedDirEnv.data());
    if (!value || !*value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    std::filesystem::path dir(value);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_pid_file(std::string_view role)
{
    auto dir = shared_directory();
    std::filesystem::create_directories(dir);
    std::filesystem::path path = dir / (std::string(role) + ".pid");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + path.string() + " for writing");
    }
#ifdef _WIN32
    out << _getpid();
#else
    out << getpid();
#endif
    out << '\n';
}

void install_signal_handlers()
{
    std::signal(SIGABRT, +[](int) {
        std::fprintf(stderr, "[coordinator] caught SIGABRT\n");
        std::fflush(stderr);
    });
    std::signal(SIGSEGV, +[](int) {
        std::fprintf(stderr, "[process] caught SIGSEGV\n");
        std::fflush(stderr);
    });
    std::signal(SIGPIPE, SIG_IGN);
}

#ifndef _WIN32

[[noreturn]] void worker_loop(int read_fd, int write_fd)
{
    install_signal_handlers();
    write_pid_file("worker");

    bool idle_notified = false;

    while (true) {
        char value = 0;
        const ssize_t bytes = ::read(read_fd, &value, 1);
        if (bytes <= 0) {
            if (!idle_notified) {
                std::fprintf(stderr, "[worker] input closed, idling for debugger attachment\n");
                std::fflush(stderr);
                idle_notified = true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (value != kPingByte) {
            std::fprintf(stderr, "[worker] unexpected byte %d\n", static_cast<int>(value));
            std::fflush(stderr);
            continue;
        }

        const char response = kPongByte;
        if (::write(write_fd, &response, 1) != 1) {
            std::perror("[worker] write failed");
        }
    }
}

[[noreturn]] void coordinator_loop(int send_fd, int recv_fd)
{
    install_signal_handlers();
    write_pid_file("coordinator");

    for (int round = 1; round <= kCrashAfterRounds; ++round) {
        const char ping = kPingByte;
        if (::write(send_fd, &ping, 1) != 1) {
            std::perror("[coordinator] write failed");
            std::exit(1);
        }

        char response = 0;
        const ssize_t bytes = ::read(recv_fd, &response, 1);
        if (bytes != 1 || response != kPongByte) {
            std::fprintf(stderr, "[coordinator] invalid pong after round %d (bytes=%zd, value=%d)\n",
                         round, bytes, static_cast<int>(response));
            std::fflush(stderr);
            std::exit(1);
        }

        std::fprintf(stderr, "[coordinator] completed round %d\n", round);
        std::fflush(stderr);
    }

    std::fprintf(stderr, "[coordinator] triggering crash after %d rounds\n", kCrashAfterRounds);
    std::fflush(stderr);
    std::abort();
}

#endif // !_WIN32

} // namespace

int main()
{
#ifdef _WIN32
    std::fprintf(stderr, "This test requires a POSIX environment.\n");
    return 1;
#else
    int to_worker[2];
    int to_coordinator[2];
    if (::pipe(to_worker) != 0 || ::pipe(to_coordinator) != 0) {
        std::perror("pipe");
        return 1;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }

    if (pid == 0) {
        ::close(to_worker[1]);
        ::close(to_coordinator[0]);
        worker_loop(to_worker[0], to_coordinator[1]);
    }

    ::close(to_worker[0]);
    ::close(to_coordinator[1]);
    coordinator_loop(to_worker[1], to_coordinator[0]);
    return 0;
#endif
}
