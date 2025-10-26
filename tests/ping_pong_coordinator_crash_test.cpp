#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

constexpr char kPing = 'P';
constexpr char kPong = 'p';
constexpr char kCrashNotice = 'C';
constexpr char kCrashAck = 'c';

void DumpStack(std::string_view label)
{
    std::array<void*, 64> frames{};
    int captured = backtrace(frames.data(), static_cast<int>(frames.size()));
    std::cerr << "\n==== Stack dump for " << label << " (pid " << getpid() << ") ====\n";
    if (captured <= 0) {
        std::cerr << "<no stack information available>\n";
        return;
    }

    char** symbols = backtrace_symbols(frames.data(), captured);
    if (!symbols) {
        std::cerr << "backtrace_symbols failed: " << std::strerror(errno) << "\n";
        return;
    }

    for (int i = 0; i < captured; ++i) {
        std::cerr << symbols[i] << '\n';
    }
    std::cerr.flush();
    std::free(symbols);
}

[[noreturn]] void CrashCoordinator()
{
    DumpStack("coordinator before abort");
    std::cerr << "Coordinator deliberately aborting after notifying worker." << std::endl;
    std::abort();
}

bool ReadChar(int fd, char& value)
{
    while (true) {
        ssize_t n = read(fd, &value, sizeof(value));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::perror("read");
            return false;
        }
        if (n == 0) {
            return false;  // EOF
        }
        return n == sizeof(value);
    }
}

bool WriteChar(int fd, char value)
{
    while (true) {
        ssize_t n = write(fd, &value, sizeof(value));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::perror("write");
            return false;
        }
        return n == sizeof(value);
    }
}

[[noreturn]] void WorkerLoop(int read_fd, int write_fd)
{
    char value = 0;
    while (ReadChar(read_fd, value)) {
        if (value == kPing) {
            if (!WriteChar(write_fd, kPong)) {
                break;
            }
        }
        else if (value == kCrashNotice) {
            DumpStack("worker responding to crash");
            if (!WriteChar(write_fd, kCrashAck)) {
                break;
            }
        }
    }

    DumpStack("worker exiting after coordinator crash");
    std::_Exit(EXIT_SUCCESS);
}

}  // namespace

int main()
{
    int coordinator_to_worker[2];
    int worker_to_coordinator[2];

    if (pipe(coordinator_to_worker) != 0 || pipe(worker_to_coordinator) != 0) {
        ::perror("pipe");
        return EXIT_FAILURE;
    }

    pid_t worker_pid = fork();
    if (worker_pid < 0) {
        ::perror("fork");
        return EXIT_FAILURE;
    }

    if (worker_pid == 0) {
        // Worker process
        close(coordinator_to_worker[1]);
        close(worker_to_coordinator[0]);
        WorkerLoop(coordinator_to_worker[0], worker_to_coordinator[1]);
    }

    // Coordinator process (parent)
    close(coordinator_to_worker[0]);
    close(worker_to_coordinator[1]);

    constexpr int kHandshakeRounds = 3;

    for (int i = 0; i < kHandshakeRounds; ++i) {
        if (!WriteChar(coordinator_to_worker[1], kPing)) {
            std::cerr << "Failed to send ping to worker." << std::endl;
            kill(worker_pid, SIGKILL);
            return EXIT_FAILURE;
        }
        char reply = 0;
        if (!ReadChar(worker_to_coordinator[0], reply) || reply != kPong) {
            std::cerr << "Worker failed to respond to ping." << std::endl;
            kill(worker_pid, SIGKILL);
            return EXIT_FAILURE;
        }
    }

    std::cerr << "Completed " << kHandshakeRounds << " ping/pong iterations." << std::endl;

    if (!WriteChar(coordinator_to_worker[1], kCrashNotice)) {
        std::cerr << "Unable to notify worker about the impending crash." << std::endl;
        kill(worker_pid, SIGKILL);
        return EXIT_FAILURE;
    }

    char acknowledgement = 0;
    if (!ReadChar(worker_to_coordinator[0], acknowledgement) || acknowledgement != kCrashAck) {
        std::cerr << "Worker did not acknowledge crash notice." << std::endl;
        kill(worker_pid, SIGKILL);
        return EXIT_FAILURE;
    }

    // Give the worker a moment to flush its logs before crashing.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CrashCoordinator();
}
