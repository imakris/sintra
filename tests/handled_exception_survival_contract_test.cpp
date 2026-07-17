//
// Sintra Handled-Exception Survival Contract Test
//
// Contract under test (docs/reference/lifecycle_hooks.md):
// `Managed_process::terminated_abnormally` is emitted by the abnormal process
// path, i.e. it is evidence that the emitting process is terminating. A
// hardware exception that a downstream handler recovers from does not
// terminate the process, so it must not trigger the abnormal-termination
// protocol: no terminated_abnormally broadcast, and the process's message
// readers keep running.
//
// Regression background: on 2026-07-16 a recovered access violation inside a
// production coordinator ran the death protocol from the first-chance
// vectored exception handler. The surviving coordinator kept running with all
// of its message readers stopped, and later session readiness and unload
// exchanges failed while process custody kept working. This test pins the
// corrected rule: a recovered exception is not evidence of process
// termination and must leave the abnormal-termination protocol untouched.
//
// The scenario is Windows-specific (first-chance SEH visibility); on other
// platforms the test passes trivially.
//

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>

#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::string_view k_failure_prefix =
    "[handled_exception_survival_contract_test] FAIL: ";

constexpr const char* k_shared_dir_env       = "SINTRA_HANDLED_EXCEPTION_SURVIVAL_DIR";
constexpr const char* k_ready_barrier        = "handled-exception-ready";
constexpr const char* k_abnormal_marker      = "child_saw_terminated_abnormally";
constexpr const char* k_parent_done_marker   = "parent_done";
constexpr const char* k_child_exited_marker  = "child_exited";

constexpr auto k_echo_timeout        = std::chrono::seconds(10);
constexpr auto k_dispatch_settle     = std::chrono::seconds(1);
constexpr auto k_child_exit_timeout  = std::chrono::seconds(10);
constexpr auto k_child_wait_for_done = std::chrono::seconds(60);

struct echo_request_t { int seq; };
struct echo_reply_t   { int seq; };

bool write_marker(const fs::path& dir, const char* name)
{
    std::ofstream out(dir / name);
    out << "1\n";
    return out.good();
}

bool wait_for_marker(
    const fs::path&           dir,
    const char*               name,
    std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(dir / name)) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return fs::exists(dir / name);
}

int child_process()
{
    sintra::test::Shared_directory shared(
        k_shared_dir_env, "handled_exception_survival");
    const fs::path dir = shared.path();

    sintra::activate_slot([](const echo_request_t& msg) {
        sintra::world() << echo_reply_t{msg.seq};
    });

    auto deactivate_abnormal = sintra::s_mproc->activate<sintra::Managed_process>(
        [dir](const sintra::Managed_process::terminated_abnormally&) {
            write_marker(dir, k_abnormal_marker);
        },
        sintra::Typed_instance_id<sintra::Managed_process>(sintra::any_remote));

    sintra::barrier(k_ready_barrier, "_sintra_all_processes");

    if (!wait_for_marker(dir, k_parent_done_marker, k_child_wait_for_done)) {
        return 1;
    }
    return write_marker(dir, k_child_exited_marker) ? 0 : 1;
}

#ifdef _WIN32

void* g_protected_page = nullptr;

// Application-level top-level filter: makes the inaccessible page writable
// and resumes the faulting instruction. It is installed before Sintra and
// must remain the process filter after Sintra initialization.
LONG WINAPI protected_page_handler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto fault_address =
        static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
    const auto page_base = reinterpret_cast<uintptr_t>(g_protected_page);
    if (fault_address < page_base || fault_address >= page_base + 4096) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD previous_protection = 0;
    if (!VirtualProtect(
            g_protected_page, 4096, PAGE_READWRITE, &previous_protection)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Returns true when the faulting write completed, which proves the exception
// was recovered and the process kept running.
bool trigger_recovered_access_violation()
{
    g_protected_page = VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (!g_protected_page) {
        return false;
    }
    bool completed = false;
    std::thread([&]() {
        volatile int* slot = static_cast<volatile int*>(g_protected_page);
        *slot = 7;
        completed = (*slot == 7);
    }).join();
    return completed;
}

int run_coordinator(const fs::path& dir)
{
    std::mutex              reply_mutex;
    std::condition_variable reply_changed;
    int                     last_reply_seq = 0;

    sintra::activate_slot([&](const echo_reply_t& msg) {
        {
            std::lock_guard lock(reply_mutex);
            last_reply_seq = msg.seq;
        }
        reply_changed.notify_all();
    });

    sintra::barrier(k_ready_barrier, "_sintra_all_processes");

    auto echo_round_trip = [&](int seq) {
        sintra::world() << echo_request_t{seq};
        std::unique_lock lock(reply_mutex);
        return reply_changed.wait_for(
            lock, k_echo_timeout, [&] { return last_reply_seq >= seq; });
    };

    bool ok = true;
    ok &= sintra::test::assert_true(
        echo_round_trip(1),
        k_failure_prefix,
        "pre-fault echo across the swarm must work");

    if (ok) {
        sintra::disable_debug_pause_for_current_process();
        ok &= sintra::test::assert_true(
            trigger_recovered_access_violation(),
            k_failure_prefix,
            "the recovered access violation must complete in-process");
    }

    if (ok) {
        // Give any (erroneous) asynchronous death dispatch time to land
        // before probing; the dispatcher itself is awaited for only 200ms
        // by the faulting thread.
        std::this_thread::sleep_for(k_dispatch_settle);

        ok &= sintra::test::assert_true(
            echo_round_trip(2),
            k_failure_prefix,
            "messaging must keep working after a recovered exception");
        ok &= sintra::test::assert_true(
            !fs::exists(dir / k_abnormal_marker),
            k_failure_prefix,
            "no terminated_abnormally may be broadcast for a recovered exception");
    }

    ok &= sintra::test::assert_true(
        write_marker(dir, k_parent_done_marker),
        k_failure_prefix,
        "the coordinator must release the child cleanup wait");
    ok &= sintra::test::assert_true(
        wait_for_marker(dir, k_child_exited_marker, k_child_exit_timeout),
        k_failure_prefix,
        "the child must finish before the coordinator reports success");

    if (!ok) {
        // With the defect present the message plane is dead and a graceful
        // sintra teardown can block without bound; the observations are
        // already reported, so end the process with an ordinary failure code.
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 1);
    }

    std::fprintf(stderr, "handled_exception_survival_contract_test PASSED\n");
    return 0;
}

#endif // _WIN32

} // namespace

int main(int argc, char* argv[])
{
#ifndef _WIN32
    (void)argc;
    (void)argv;
    std::fprintf(stderr,
        "handled_exception_survival_contract_test skipped: "
        "first-chance SEH recovery is Windows-specific\n");
    return 0;
#else
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared(
        k_shared_dir_env, "handled_exception_survival");
    const fs::path dir = shared.path();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(child_process);

    SetUnhandledExceptionFilter(protected_page_handler);
    sintra::init(argc, argv, processes);

    const auto filter_after_init =
        SetUnhandledExceptionFilter(protected_page_handler);
    if (filter_after_init != protected_page_handler) {
        std::fprintf(
            stderr,
            "%sSintra must not replace the host's unhandled-exception filter\n",
            k_failure_prefix.data());
        return 1;
    }

    if (is_spawned) {
        return 0;
    }
    return run_coordinator(dir);
#endif
}
