// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#if defined(__APPLE__)

#include "test_utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using External_child = sintra::Native_family_external_child;
constexpr auto k_child_iid = sintra::compose_instance(67u, 1ull);

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

class Direct_child
{
public:
    bool start()
    {
        int gate[2];
        if (pipe(gate) != 0) {
            return false;
        }
        m_pid = fork();
        if (m_pid == 0) {
            close(gate[1]);
            alarm(12);
            char value;
            while (read(gate[0], &value, 1) < 0 && errno == EINTR) {}
            _exit(0);
        }
        close(gate[0]);
        if (m_pid < 0) {
            close(gate[1]);
            return false;
        }
        m_gate = gate[1];
        return true;
    }

    pid_t pid() const { return m_pid; }

    void release()
    {
        if (m_gate >= 0) {
            close(m_gate);
            m_gate = -1;
        }
    }

    bool exited_without_reaping()
    {
        const auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline) {
            siginfo_t exit{};
            if (waitid(P_PID, m_pid, &exit, WEXITED | WNOHANG | WNOWAIT) != 0) {
                return false;
            }
            if (exit.si_pid == m_pid) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool reap()
    {
        int status = 0;
        pid_t result;
        do {
            result = waitpid(m_pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result != m_pid) {
            return false;
        }
        m_pid = 0;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    ~Direct_child()
    {
        release();
        if (m_pid > 0) {
            // This fixture retains the sole wait for its original child.
            kill(m_pid, SIGKILL);
            while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

private:
    pid_t m_pid = 0;
    int   m_gate = -1;
};

bool start_family(int argc, char* argv[])
{
    if (!check(!sintra::activate_native_family(), "Darwin cannot claim transitive containment") ||
        !check(sintra::activate_native_family(sintra::Native_family_scope::OWNED_CHILDREN),
            "explicit owned-child activation before init"))
    {
        return false;
    }
    const auto status = sintra::native_family_status();
    if (!check(status.active && status.scope == sintra::Native_family_scope::OWNED_CHILDREN &&
            status.native_error == 0, "status identifies the activated scope"))
    {
        return false;
    }
    sintra::init(argc, argv);
    return check(!sintra::activate_native_family(sintra::Native_family_scope::OWNED_CHILDREN),
        "activation is restricted to before init");
}

bool wait_family_empty(const sintra::Managed_child_native_change_signal& changes)
{
    const auto deadline = Clock::now() + 4s;
    while (Clock::now() < deadline) {
        const auto generation = changes.generation();
        if (sintra::native_family_status().native_empty) {
            return true;
        }
        changes.wait_for_change(generation, deadline);
    }
    return false;
}

bool observes_only(
    const sintra::Native_family_status& status,
    std::initializer_list<std::uint32_t> process_ids)
{
    return std::is_permutation(status.observed_process_ids.begin(), status.observed_process_ids.end(),
        process_ids.begin(), process_ids.end());
}

sintra::Spawn_options managed_child_options(int argc, char* argv[])
{
    sintra::Spawn_options options;
    options.binary_path = sintra::test::get_binary_path(argc, argv);
    options.args = {"--owned-family-case", "managed-child"};
    options.process_instance_id = k_child_iid;
    options.lifetime.enable_lifeline = false;
    return options;
}

bool run_settlement(int argc, char* argv[])
{
    if (!start_family(argc, argv)) {
        return false;
    }
    sintra::Managed_child_native_change_signal changes;
    sintra::observe_native_family_changes(changes);
    auto external = External_child::reserve();
    Direct_child child;
    if (!check(external.admission() == External_child::Admission::ADMITTED && child.start(),
            "external reservation precedes original birth") ||
        !check(external.publish_created(child.pid(), 0), "publish original child with local observer"))
    {
        return false;
    }
    auto custody = sintra::spawn_swarm_process(managed_child_options(argc, argv));
    const auto native = custody.native_snapshot();
    const auto created = custody.status();
    if (!check(native.size() == 1 && native.front().state == sintra::Managed_child_native_state::RUNNING &&
            native.front().occurrence.process_instance_id == k_child_iid && native.front().ownership_ready,
            "managed original occurrence has native custody"))
    {
        return false;
    }
    bool valid = check(created.admitted_occurrences == 1 && created.created_occurrences == 1 &&
        created.exited_occurrences == 0, "one managed admission creates exactly one live original");
    auto pending = sintra::s_mproc->accept_child_custody();
    {
        auto launch = sintra::s_mproc->admit_child_custody_occurrence(
            pending, sintra::compose_instance(69u, 1ull), 0);
        sintra::s_mproc->request_child_custody_release(pending);
        sintra::close_native_family_admission();
        const auto status = sintra::native_family_status();
        if (!check(status.admission_closed && status.pending_launches && !status.native_empty &&
                status.native_error == 0,
                "cancelled pending setup blocks closed-family completion") ||
            !check(External_child::reserve().admission() == External_child::Admission::CLOSED,
                "closure rejects later external birth") ||
            !check(observes_only(status, {(std::uint32_t)child.pid(), (std::uint32_t)native.front().pid}),
                "closed family retains both original native identities exactly once"))
        {
            return false;
        }
    }
    const auto settled_setup = sintra::native_family_status();
    valid &= check(!settled_setup.pending_launches && !settled_setup.native_empty &&
        observes_only(settled_setup, {(std::uint32_t)child.pid(), (std::uint32_t)native.front().pid}),
        "no-birth settlement removes pending setup");
    valid &= check(!external.observe_reaped(), "a live original cannot provide a terminal receipt");
    const auto action = custody.request_native_termination(native.front().occurrence, Clock::now() + 3s);
    valid &= check(action.admission == sintra::Managed_child_native_admission::STARTED,
        "exact managed custody admits its termination");
    const auto released = custody.terminate_until(Clock::now() + 4s);
    valid &= check(released.release_state == sintra::Managed_child_release_state::complete &&
        released.admitted_occurrences == 1 && released.created_occurrences == 1 &&
        released.exited_occurrences == 1, "managed occurrence settles through sole reaper");
    valid &= check(observes_only(sintra::native_family_status(), {(std::uint32_t)child.pid()}),
        "settled managed original leaves only the external native identity");
    child.release();
    valid &= check(child.exited_without_reaping(), "external child exit remains available to its own waiter");
    valid &= check(!sintra::native_family_status().native_empty,
        "exit alone does not replace the external owner's reap receipt");
    valid &= check(child.reap(), "external owner retains its exact native wait result");
    valid &= check(external.observe_reaped(), "retained original exit accepts the completed owner wait");
    valid &= check(wait_family_empty(changes), "closed owned-child family becomes empty after all receipts");
    const auto empty = sintra::native_family_status();
    valid &= check(empty.admission_closed && !empty.pending_launches && empty.native_error == 0 &&
        empty.observed_process_ids.empty(), "settled family retains no pending or native identities");
    valid &= check(sintra::shutdown(), "settled family permits runtime shutdown");
    return valid;
}

bool run_closed_admission(int argc, char* argv[])
{
    if (!start_family(argc, argv)) {
        return false;
    }
    sintra::close_native_family_admission();
    auto custody = sintra::spawn_swarm_process(managed_child_options(argc, argv));
    bool valid = check(!custody, "closed family rejects the managed spawn request");

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(-1, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    valid &= check(waited == -1 && errno == ECHILD, "post-close managed spawn creates no native child");
    const auto family = sintra::native_family_status();
    valid &= check(family.admission_closed && !family.pending_launches && family.native_empty &&
        family.native_error == 0 && family.observed_process_ids.empty(),
        "rejected managed spawn leaves closed family accounting empty");
    valid &= check(sintra::shutdown(), "rejected managed spawn permits complete shutdown");
    return valid;
}

bool run_lost_managed_wait(int argc, char* argv[])
{
    if (!start_family(argc, argv)) {
        return false;
    }
    auto custody = sintra::spawn_swarm_process(managed_child_options(argc, argv));
    const auto native = custody.native_snapshot();
    if (!check(native.size() == 1 && native.front().state == sintra::Managed_child_native_state::RUNNING &&
            native.front().pid > 0 && native.front().ownership_ready,
            "managed original is running before loss of its sole wait"))
    {
        return false;
    }
    const auto pid = native.front().pid;
    sintra::close_native_family_admission();
    const auto before = sintra::native_family_status();
    bool valid = check(before.admission_closed && !before.pending_launches && !before.native_empty &&
        before.native_error == 0 && observes_only(before, {(std::uint32_t)pid}),
        "closed family accounts for the managed original before wait loss");
    {
        // Exclude Sintra's sole reaper while another waiter consumes the real
        // terminal result, so its next exact wait must observe ECHILD.
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_spawned_child_pids_mutex);
        if (!check(kill(pid, SIGKILL) == 0, "terminate original while the sole reaper is excluded")) {
            return false;
        }
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (!check(waited == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
                "competing waiter consumes the exact original exit"))
        {
            return false;
        }
    }
    sintra::s_mproc->reap_finished_children();
    const auto lost = sintra::native_family_status();
    valid &= check(lost.native_error == ECHILD && !lost.native_empty && !lost.pending_launches &&
        lost.observed_process_ids.empty(), "lost sole wait is an error even after the native inventory is empty");
    const auto exited = custody.native_snapshot();
    valid &= check(exited.size() == 1 && exited.front().occurrence == native.front().occurrence &&
        exited.front().state == sintra::Managed_child_native_state::EXITED &&
        !exited.front().native_exit_status_available, "lost managed wait cannot fabricate a terminal result");
    const auto released = custody.release_until(Clock::now() + 4s);
    valid &= check(released.release_state == sintra::Managed_child_release_state::complete &&
        released.admitted_occurrences == 1 && released.created_occurrences == 1 &&
        released.exited_occurrences == 1, "wait loss still permits release of the original managed custody");
    const auto after_release = sintra::native_family_status();
    valid &= check(after_release.native_error == ECHILD && !after_release.native_empty &&
        !after_release.pending_launches && after_release.observed_process_ids.empty(),
        "managed custody release cannot clear the unconfirmed family result");
    valid &= check(!sintra::shutdown(), "lost managed sole wait prevents complete shutdown");
    const auto after_shutdown = sintra::native_family_status();
    valid &= check(after_shutdown.native_error == ECHILD && !after_shutdown.native_empty,
        "managed wait loss remains sticky after shutdown attempts");
    return valid;
}

bool run_unpublished(int argc, char* argv[])
{
    if (!start_family(argc, argv)) {
        return false;
    }
    auto external = External_child::reserve();
    Direct_child child;
    if (!check(external.admission() == External_child::Admission::ADMITTED && child.start(),
            "reservation precedes unpublished native birth"))
    {
        return false;
    }
    bool valid = check(!external.publish_created(0, 0), "failed publication retains the pending reservation");
    sintra::close_native_family_admission();
    const auto pending = sintra::native_family_status();
    valid &= check(pending.admission_closed && pending.pending_launches && !pending.native_empty &&
        pending.observed_process_ids.empty(), "unpublished original cannot disappear from closure");
    child.release();
    valid &= check(child.reap(), "creator reaps failed publication before cancellation");
    external.cancel_uncreated();
    const auto settled = sintra::native_family_status();
    valid &= check(settled.native_empty && !settled.pending_launches && settled.native_error == 0 &&
        settled.observed_process_ids.empty(),
        "original creator receipt settles unpublished birth");
    valid &= check(sintra::shutdown(), "failed publication settlement permits shutdown");
    return valid;
}

bool run_lost_observation(int argc, char* argv[], bool lost_owner)
{
    if (!start_family(argc, argv)) {
        return false;
    }
    auto external = External_child::reserve();
    Direct_child child;
    if (!check(child.start() && external.publish_created(child.pid(), 0), "original observation established")) {
        return false;
    }
    sintra::close_native_family_admission();
    if (lost_owner) {
        external.handoff_wait_authority();
    }
    else {
        auto& family = sintra::detail::native_process_family();
        std::lock_guard<std::mutex> lock(family.m_mutex);
        auto& retained = family.m_external_children.front();
        close(retained->native);
        retained->native = -1;
    }
    child.release();
    bool valid = check(child.reap(), "fixture still performs the original wait after injected loss");
    if (!lost_owner) {
        valid &= check(!external.observe_reaped(), "unavailable retained observer cannot establish exit");
    }
    const auto status = sintra::native_family_status();
    valid &= check(status.native_error != 0 && !status.native_empty,
        "lost original observation stays unconfirmed after PID disappearance");
    valid &= check(!sintra::shutdown(), "unconfirmed family cannot report complete shutdown");
    return valid;
}

bool run_case(const std::string& binary, const char* name)
{
    const auto child = fork();
    if (child == 0) {
        execl(binary.c_str(), binary.c_str(), "--owned-family-case", name, static_cast<char*>(nullptr));
        _exit(2);
    }
    if (!check(child > 0, "launch isolated scope contract")) {
        return false;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "FAIL: owned-family case %s status=%d\n", name, status);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    const auto mode = sintra::test::get_argv_value(argc, argv, "--owned-family-case");
    if (mode == "managed-child") {
        alarm(12);
        while (true) {
            pause();
        }
    }
    if (!mode.empty()) {
        alarm(20);
        bool valid = false;
        if (mode == "settlement")        { valid = run_settlement(argc, argv);                    }
        if (mode == "closed-admission")  { valid = run_closed_admission(argc, argv);              }
        if (mode == "lost-managed-wait") { valid = run_lost_managed_wait(argc, argv);             }
        if (mode == "unpublished")       { valid = run_unpublished(argc, argv);                   }
        if (mode == "lost-owner")        { valid = run_lost_observation(argc, argv, true);        }
        if (mode == "lost-observer")     { valid = run_lost_observation(argc, argv, false);       }
        // Loss cases deliberately leave incomplete runtime custody. The driver
        // owns their process exit; all native fixture children are already reaped.
        std::_Exit(valid ? 0 : 1);
    }
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "owned_family");
    const auto binary = sintra::test::get_binary_path(argc, argv);
    bool valid = true;
    for (const auto* name : {"settlement", "closed-admission", "lost-managed-wait",
             "unpublished", "lost-owner", "lost-observer"})
    {
        valid &= run_case(binary, name);
    }
    return valid ? 0 : 1;
}

#else

int main()
{
    // OWNED_CHILDREN is a Darwin capability; other backends retain TRANSITIVE.
    return sintra::activate_native_family(sintra::Native_family_scope::OWNED_CHILDREN) ? 1 : 0;
}

#endif
