//
// Sintra Reader-Thread Fault Contract Test
//
// Contract under test (docs/reference/lifecycle_hooks.md,
// docs/reference/announce_fatal_windows_exception.md):
//
// 1. A terminal hardware fault on a Sintra-owned thread is reported. Windows
//    raises a C signal for a hardware fault only through the UCRT's
//    __scrt_common_main_seh filter, which exists on the main thread alone, so a
//    fault on a ring reader thread - where application message handlers run -
//    reaches no CRT signal path. Sintra places its own structured-exception
//    frame on the threads it owns so that such a fault still runs the
//    abnormal-termination dispatch: the coordinator observes reason::crash with
//    status SIGSEGV, and a barrier the faulted peer was part of stops waiting
//    for it instead of hanging.
//
// 2. The host keeps the final say. Sintra's filter consults
//    UnhandledExceptionFilter before declaring anything, so a fault that the
//    host's own filter repairs is not a death: no terminated_abnormally, and the
//    reader that faulted keeps running. This is the rule stated by
//    handled_exception_survival_contract_test, which covers it for an
//    application thread; this test covers it for a Sintra-owned one.
//
// Both scenarios are Windows-specific and additionally require the guard to be
// compiled in (SINTRA_HAS_SEH); elsewhere the test passes trivially.
//

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
    "[reader_thread_fault_contract_test] FAIL: ";

constexpr const char* k_shared_dir_env = "SINTRA_READER_THREAD_FAULT_DIR";
constexpr const char* k_ready_barrier  = "reader-thread-fault-ready";

constexpr const char* k_abnormal_marker = "child_saw_terminated_abnormally";
constexpr const char* k_repaired_marker = "child_repaired_fault_in_handler";

constexpr auto k_reply_timeout   = std::chrono::seconds(10);
constexpr auto k_crash_timeout   = std::chrono::seconds(20);
constexpr auto k_dispatch_settle = std::chrono::seconds(1);
constexpr auto k_child_lifetime  = std::chrono::seconds(60);

// repair_t makes the child touch a page its own filter will make writable, so
// the fault is recovered. fatal_t makes it fault on a page nothing repairs.
struct repair_t {};
struct fatal_t  {};
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

#if defined(_WIN32) && SINTRA_HAS_SEH

void* g_repairable_page = nullptr;

// The host's terminal filter. It repairs exactly one page and declines
// everything else, so the repair_t fault is recovered and the fatal_t fault is
// not. Installed in the child, before sintra::init.
LONG WINAPI host_filter(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !g_repairable_page) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto fault_address =
        static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
    const auto page_base = reinterpret_cast<uintptr_t>(g_repairable_page);
    if (fault_address < page_base || fault_address >= page_base + 4096) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD previous_protection = 0;
    if (!VirtualProtect(
            g_repairable_page, 4096, PAGE_READWRITE, &previous_protection)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

int child_process()
{
    sintra::test::Shared_directory shared(
        k_shared_dir_env, "reader_thread_fault");
    const fs::path dir = shared.path();

    g_repairable_page = VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);

    // Evidence that no death is declared for the repaired fault.
    auto deactivate_abnormal = sintra::s_mproc->activate<sintra::Managed_process>(
        [dir](const sintra::Managed_process::terminated_abnormally&) {
            write_marker(dir, k_abnormal_marker);
        },
        sintra::Typed_instance_id<sintra::Managed_process>(sintra::any_remote));

    sintra::activate_slot([](const echo_request_t& msg) {
        sintra::world() << echo_reply_t{msg.seq};
    });

    // Both handlers run on a ring reader thread, which is the point.
    sintra::activate_slot([dir](const repair_t&) {
        volatile int* slot = static_cast<volatile int*>(g_repairable_page);
        *slot = 7;
        if (*slot == 7) {
            write_marker(dir, k_repaired_marker);
        }
    });

    sintra::activate_slot([](const fatal_t&) {
        sintra::disable_debug_pause_for_current_process();
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    });

    sintra::barrier(k_ready_barrier, "_sintra_all_processes");

    // The coordinator ends this process by faulting it.
    std::this_thread::sleep_for(k_child_lifetime);
    return 0;
}

// Observed state lives for the life of the process on purpose. Both the slot
// and the lifecycle handler stay registered through Sintra's own teardown, and
// teardown itself emits lifecycle events, so anything they touch must outlive
// every scope in this file.
struct Observations
{
    std::mutex              reply_mutex;
    std::condition_variable reply_changed;
    int                     last_reply_seq = 0;

    std::mutex              crash_mutex;
    std::condition_variable crash_changed;
    int                     crash_events = 0;
    int                     crash_status = 0;
};

Observations& observations()
{
    static Observations state;
    return state;
}

int run_coordinator(const fs::path& dir)
{
    Observations& obs = observations();

    sintra::activate_slot([](const echo_reply_t& msg) {
        Observations& state = observations();
        {
            std::lock_guard lock(state.reply_mutex);
            state.last_reply_seq = msg.seq;
        }
        state.reply_changed.notify_all();
    });

    sintra::set_lifecycle_handler(
        [](const sintra::process_lifecycle_event& event) {
            Observations& state = observations();
            {
                std::lock_guard lock(state.crash_mutex);
                if (event.why == sintra::process_lifecycle_event::reason::crash) {
                    ++state.crash_events;
                    state.crash_status = event.status;
                }
            }
            state.crash_changed.notify_all();
        });

    sintra::barrier(k_ready_barrier, "_sintra_all_processes");

    auto echo_round_trip = [&obs](int seq) {
        sintra::world() << echo_request_t{seq};
        std::unique_lock lock(obs.reply_mutex);
        return obs.reply_changed.wait_for(
            lock, k_reply_timeout, [&obs, seq] { return obs.last_reply_seq >= seq; });
    };

    bool ok = true;
    ok &= sintra::test::assert_true(
        echo_round_trip(1),
        k_failure_prefix,
        "pre-fault echo across the swarm must work");

    // --- scenario 2: a fault the host repairs is not a death ---------------
    if (ok) {
        sintra::world() << repair_t{};
        ok &= sintra::test::assert_true(
            wait_for_marker(dir, k_repaired_marker, k_reply_timeout),
            k_failure_prefix,
            "the host filter must repair a fault taken on a reader thread");
    }
    if (ok) {
        std::this_thread::sleep_for(k_dispatch_settle);
        ok &= sintra::test::assert_true(
            echo_round_trip(2),
            k_failure_prefix,
            "the faulted reader must keep running after the host repairs it");
        ok &= sintra::test::assert_true(
            !fs::exists(dir / k_abnormal_marker),
            k_failure_prefix,
            "a repaired fault must not broadcast terminated_abnormally");
        std::lock_guard lock(obs.crash_mutex);
        ok &= sintra::test::assert_true(
            obs.crash_events == 0,
            k_failure_prefix,
            "a repaired fault must not raise a crash lifecycle event");
    }

    // --- scenario 1: an unrepaired fault is reported -----------------------
    if (ok) {
        sintra::world() << fatal_t{};
        std::unique_lock lock(obs.crash_mutex);
        ok &= sintra::test::assert_true(
            obs.crash_changed.wait_for(
                lock, k_crash_timeout, [&obs] { return obs.crash_events > 0; }),
            k_failure_prefix,
            "a fault on a reader thread must reach the coordinator as a crash");
        if (ok) {
            ok &= sintra::test::assert_true(
                obs.crash_status == SIGSEGV,
                k_failure_prefix,
                "the crash status must be SIGSEGV for an access violation");
            ok &= sintra::test::assert_true(
                obs.crash_events == 1,
                k_failure_prefix,
                "the crash must be reported exactly once");
        }
    }

    if (!ok) {
        // With the defect present the message plane may be dead and a graceful
        // teardown can block without bound. The observations are already
        // reported, so end with an ordinary failure code.
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 1);
    }

    std::fprintf(stderr, "reader_thread_fault_contract_test PASSED\n");
    return 0;
}

#endif // _WIN32 && SINTRA_HAS_SEH

} // namespace

int main(int argc, char* argv[])
{
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    std::fprintf(stderr,
        "reader_thread_fault_contract_test skipped: POSIX delivers a hardware "
        "fault as a signal on the faulting thread, so the process-wide handler "
        "already covers every thread\n");
    return 0;
#elif !SINTRA_HAS_SEH
    (void)argc;
    (void)argv;
    std::fprintf(stderr,
        "reader_thread_fault_contract_test skipped: built without "
        "SINTRA_HAS_SEH, so sintra places no structured-exception frame on the "
        "threads it owns\n");
    return 0;
#else
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared(
        k_shared_dir_env, "reader_thread_fault");
    const fs::path dir = shared.path();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(child_process);

    // The host owns the terminal filter, before and after sintra::init.
    SetUnhandledExceptionFilter(host_filter);
    sintra::init(argc, argv, processes);

    const auto filter_after_init = SetUnhandledExceptionFilter(host_filter);
    if (filter_after_init != host_filter) {
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
