// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// The native family reaper enumerates each owned thread's /proc children list,
// and activation refuses when that list cannot be read. This test does not ask
// whether the list is readable and then expect the matching outcome -- that
// would only confirm its own premise. Its oracle is a child the fixture
// creates itself and can prove exists, and it holds both outcomes to it: an
// activated family must observe that exact child and then prove it empty once
// it exits, and a refused activation must be unable to observe it at all.
#include <sintra/sintra.h>

#if defined(__linux__)

#include <sintra/detail/process/native_process_family.h>

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

/// Ground truth about a process this fixture created itself, read from /proc
/// and independent of anything the family reports. 'R' or 'S' is alive, 'Z' is
/// an unreaped corpse, '\0' is gone.
char process_state(pid_t pid)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) {
        return '\0';
    }
    // The comm field is parenthesised and may itself contain spaces and
    // parentheses, so the state is the character two past its last ')'.
    const auto comm_end = line.rfind(')');
    if (comm_end == std::string::npos || comm_end + 2 >= line.size()) {
        return '\0';
    }
    return line[comm_end + 2];
}

/// A real child owned outright by this fixture. It blocks reading a pipe it
/// cannot write to, so it exits exactly when the fixture closes the other end,
/// and never before.
class Fixture_child
{
public:
    /// An active family owns every child wait. Only a fixture that never handed
    /// that authority over may reap its own child.
    explicit Fixture_child(bool owns_wait): m_owns_wait(owns_wait) {}

    Fixture_child(const Fixture_child&) = delete;
    Fixture_child& operator=(const Fixture_child&) = delete;

    bool start()
    {
        if (pipe(m_release) != 0) {
            return false;
        }
        m_pid = fork();
        if (m_pid < 0) {
            return false;
        }
        if (m_pid == 0) {
            close(m_release[1]);
            for (;;) {
                char byte;
                const ssize_t received = read(m_release[0], &byte, 1);
                if (received == 0 || (received < 0 && errno != EINTR)) {
                    _exit(0);
                }
            }
        }
        close(m_release[0]);
        m_release[0] = -1;
        return true;
    }

    void release()
    {
        if (m_release[1] >= 0) {
            close(m_release[1]);
            m_release[1] = -1;
        }
    }

    /// After an observed reap the number is no longer this fixture's to signal.
    void disown() { m_pid = 0; }

    pid_t pid() const { return m_pid; }

    ~Fixture_child()
    {
        release();
        if (m_pid <= 0) {
            return;
        }
        kill(m_pid, SIGKILL);
        if (m_owns_wait) {
            while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

private:
    bool  m_owns_wait;
    pid_t m_pid = 0;
    int   m_release[2] = {-1, -1};
};

/// The reaper's own enumeration, run directly by the fixture: every owned
/// thread's child list, opened through the same authority activation consults.
/// `readable` reports whether any of those lists could be read at all.
bool enumeration_observes(pid_t subject, bool& readable)
{
    readable = false;
    bool observed = false;
    std::error_code directory_error;
    const fs::directory_iterator tasks(sintra::detail::k_owned_thread_root, directory_error);
    if (directory_error) {
        return false;
    }
    for (const auto& task : tasks) {
        std::ifstream children = sintra::detail::open_owned_thread_children(task.path());
        if (!children) {
            continue;
        }
        readable = true;
        pid_t child = 0;
        while (children >> child) {
            observed |= child == subject;
        }
    }
    return observed;
}

bool contains(const std::vector<std::uint32_t>& members, pid_t pid)
{
    const auto target = static_cast<std::uint32_t>(pid);
    return std::find(members.begin(), members.end(), target) != members.end();
}

int run_activated(int argc, char* argv[])
{
    sintra::init(argc, argv);
    Fixture_child child(false);
    bool valid = check(child.start(), "fixture creates a real child of its own");
    const pid_t subject = child.pid();
    const char birth_state = process_state(subject);
    valid &= check(birth_state == 'R' || birth_state == 'S',
        "fixture child provably exists before the family is asked about it");
    sintra::close_native_family_admission();

    // Observing the family is what drives its reaper, so the wait is the poll.
    sintra::Native_family_status observation;
    const auto observed_deadline = Clock::now() + 8s;
    bool observed = false;
    while (Clock::now() < observed_deadline) {
        observation = sintra::native_family_status();
        observed = contains(observation.observed_process_ids, subject);
        if (observed) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    valid &= check(observed, "activated family observes the exact child the fixture created");
    valid &= check(!observation.native_empty, "a family holding a live child is not empty");

    child.release();
    bool empty = false;
    const auto empty_deadline = Clock::now() + 8s;
    while (Clock::now() < empty_deadline) {
        observation = sintra::native_family_status();
        if (observation.native_empty) {
            empty = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    valid &= check(empty, "activated family reaps the exited child and concludes it is empty");
    if (empty) {
        valid &= check(process_state(subject) == '\0',
            "the empty claim is truthful: no corpse of that child is left unwaited");
        child.disown();
    }
    valid &= check(observation.native_error == 0,
        "an activated family that completed this cycle reports no failure");
    std::fprintf(stderr, "activated: child=%d observed=%d empty=%d error=%u\n",
        static_cast<int>(subject), observed ? 1 : 0, empty ? 1 : 0, observation.native_error);
    valid &= check(sintra::shutdown(), "prerequisite fixture releases its runtime");
    return valid ? 0 : 1;
}

int run_refused()
{
    const auto status = sintra::native_family_status();
    bool valid = check(!status.active && !status.native_empty && status.native_error != 0 &&
        !status.failed_operation.empty(),
        "a refused activation reports its failure without claiming an empty family");

    Fixture_child child(true);
    valid &= check(child.start(), "fixture creates a real child of its own");
    const char birth_state = process_state(child.pid());
    valid &= check(birth_state == 'R' || birth_state == 'S',
        "fixture child provably exists before the enumeration is asked about it");

    bool readable = false;
    const bool observed = enumeration_observes(child.pid(), readable);
    std::fprintf(stderr, "refused: error=%u operation=%s any_list_readable=%d\n",
        status.native_error, status.failed_operation.c_str(), readable ? 1 : 0);
    // The guarantee refused must be genuinely unobtainable. Fork and observe
    // succeeding here would mean the product newly refuses to start in an
    // environment where its reaper works.
    valid &= check(!observed,
        "activation refused although the reaper's own enumeration observes a child that provably exists");
    return valid ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::activate_native_family() ? run_activated(argc, argv) : run_refused();
}

#else

#include <cstdio>

int main()
{
    // The per-thread child list is a Linux prerequisite. Every other platform
    // observes its native family by other means and admits on other grounds.
    std::printf("per-thread child list is a Linux prerequisite; nothing to admit on here\n");
    return 0;
}

#endif
