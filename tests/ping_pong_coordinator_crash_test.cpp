#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::size_t kPingPongIterations = 5;

[[noreturn]] void worker_process(int read_fd, int write_fd)
{
    std::cerr << "worker: ready (pid=" << static_cast<long long>(::getpid()) << ")" << std::endl;

    std::size_t received = 0;
    char buffer = 0;

    while (true) {
        const ssize_t n = ::read(read_fd, &buffer, 1);
        if (n <= 0) {
            std::cerr << "worker: input closed after " << received << " messages; pausing" << std::endl;
            // keep the process alive so debuggers can inspect it
            while (true) {
                ::pause();
            }
        }

        if (buffer == 'P') {
            ++received;
            const char response = 'O';
            if (::write(write_fd, &response, 1) < 0) {
                std::perror("worker write");
                std::_Exit(EXIT_FAILURE);
            }
        }
    }
}

void coordinator_loop(int write_fd, int read_fd, pid_t child_pid)
{
    std::cout << "WORKER_PID " << static_cast<long long>(child_pid) << std::endl;
    std::cout.flush();

    if (const char* pid_file = std::getenv("SINTRA_WORKER_PID_FILE")) {
        if (*pid_file) {
            std::ofstream out(pid_file, std::ios::binary | std::ios::trunc);
            if (out) {
                out << static_cast<long long>(child_pid) << '\n';
            }
        }
    }

    const char ping = 'P';
    for (std::size_t i = 0; i < kPingPongIterations; ++i) {
        if (::write(write_fd, &ping, 1) != 1) {
            std::perror("coordinator write");
            std::abort();
        }

        char buffer = 0;
        if (::read(read_fd, &buffer, 1) <= 0) {
            std::perror("coordinator read");
            std::abort();
        }

        if (buffer != 'O') {
            std::cerr << "unexpected response: " << buffer << std::endl;
            std::abort();
        }
    }

    std::cerr << "coordinator: crashing on purpose" << std::endl;
    std::abort();
}

} // namespace

int main()
{
    int to_worker[2] = {-1, -1};
    int to_coordinator[2] = {-1, -1};
    if (::pipe(to_worker) != 0) {
        std::perror("pipe to worker");
        return EXIT_FAILURE;
    }
    if (::pipe(to_coordinator) != 0) {
        std::perror("pipe to coordinator");
        return EXIT_FAILURE;
    }

    const pid_t child = ::fork();
    if (child == -1) {
        std::perror("fork");
        return EXIT_FAILURE;
    }

    if (child == 0) {
        ::close(to_worker[1]);
        ::close(to_coordinator[0]);
        worker_process(to_worker[0], to_coordinator[1]);
    }

    // parent (coordinator)
    ::close(to_worker[0]);
    ::close(to_coordinator[1]);

    coordinator_loop(to_worker[1], to_coordinator[0], child);
    return EXIT_SUCCESS;
}
