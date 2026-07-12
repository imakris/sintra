//
// Managed-child recovery occurrence-isolation baseline (R6).
//

#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_nonce_env = "SINTRA_R6_NONCE";
constexpr std::string_view k_ledger_0 = "occurrence_0.complete";
constexpr std::string_view k_ledger_1 = "occurrence_1.complete";
constexpr std::string_view k_crash_0 = "crash_occurrence_0.complete";
constexpr std::string_view k_release_1 = "release_occurrence_1.complete";
constexpr std::string_view k_finalized_1 = "finalized_occurrence_1.complete";
constexpr std::string_view k_witness_name = "managed_child_r6_recovery_witness";
constexpr auto k_watchdog_timeout = 25s;
constexpr uint64_t k_witness_instance_index = 0x6a31u;

struct Recovery_witness : sintra::Derived_transceiver<Recovery_witness>
{
    explicit Recovery_witness(sintra::instance_id_type iid)
        : sintra::Derived_transceiver<Recovery_witness>("", iid)
    {}

    uint32_t identify_occurrence(uint32_t token)
    {
        return (sintra::s_recovery_occurrence << 16) ^ token;
    }
    SINTRA_RPC_STRICT(identify_occurrence)
};

struct Occurrence_ledger
{
    std::string nonce;
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    sintra::instance_id_type witness_iid = sintra::invalid_instance_id;
    uint32_t occurrence = std::numeric_limits<uint32_t>::max();
    int pid = -1;
    bool start_stamp_available = false;
    uint64_t start_stamp = 0;
    std::string witness_name;
};

struct Custody_tuple_facts
{
    bool record_found = false;
    bool occurrence_count_exact = false;
    bool predecessor_occurrence = false;
    bool predecessor_setup = false;
    bool predecessor_pid = false;
    bool predecessor_stamp_available = false;
    bool predecessor_stamp = false;
    bool predecessor_publication_retired = false;
    bool predecessor_communication_retired = false;
    bool predecessor_exit_confirmed = false;
    bool replacement_occurrence = false;
    bool replacement_setup = false;
    bool replacement_pid = false;
    bool replacement_stamp_available = false;
    bool replacement_stamp = false;
    bool replacement_created = false;
    bool replacement_not_exited = false;

    bool complete() const
    {
        return record_found && occurrence_count_exact &&
            predecessor_occurrence && predecessor_setup && predecessor_pid &&
            predecessor_stamp_available && predecessor_stamp &&
            predecessor_publication_retired &&
            predecessor_communication_retired && predecessor_exit_confirmed &&
            replacement_occurrence && replacement_setup && replacement_pid &&
            replacement_stamp_available && replacement_stamp &&
            replacement_created && replacement_not_exited;
    }
};

struct Outbound_hold
{
    std::atomic<sintra::instance_id_type> expected{sintra::invalid_instance_id};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

Outbound_hold s_outbound_hold;

#ifndef _WIN32
struct Reap_observation
{
    std::atomic<pid_t> pid_0{-1};
    std::atomic<pid_t> pid_1{-1};
    std::atomic<uint32_t> count_0{0};
    std::atomic<uint32_t> count_1{0};
    std::atomic<int> status_0{0};
    std::atomic<int> status_1{0};
};

Reap_observation s_reaps;
#endif

fs::path marker(const fs::path& dir, std::string_view name)
{
    return dir / std::string(name);
}

std::optional<Occurrence_ledger> read_ledger(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    Occurrence_ledger ledger;
    bool complete = false;
    std::string line;
    try {
        while (std::getline(in, line)) {
            const auto separator = line.find('=');
            if (separator == std::string::npos) {
                return std::nullopt;
            }
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (key == "nonce") {
                ledger.nonce = value;
            }
            else if (key == "piid") {
                ledger.process_iid = static_cast<sintra::instance_id_type>(std::stoull(value));
            }
            else if (key == "witness_iid") {
                ledger.witness_iid = static_cast<sintra::instance_id_type>(std::stoull(value));
            }
            else if (key == "occurrence") {
                ledger.occurrence = static_cast<uint32_t>(std::stoul(value));
            }
            else if (key == "pid") {
                ledger.pid = std::stoi(value);
            }
            else if (key == "start_stamp_available") {
                ledger.start_stamp_available = value == "1";
            }
            else if (key == "start_stamp") {
                ledger.start_stamp = std::stoull(value);
            }
            else if (key == "witness_name") {
                ledger.witness_name = value;
            }
            else if (key == "complete") {
                complete = value == "1";
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }

    if (!complete || ledger.nonce.empty() || ledger.pid <= 0 || ledger.witness_name.empty()) {
        return std::nullopt;
    }
    return ledger;
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

std::optional<sintra::instance_id_type> resolve_publicly(const std::string& name)
{
    try {
        return sintra::Coordinator::rpc_resolve_instance(sintra::s_coord_id, name);
    }
    catch (...) {
        return std::nullopt;
    }
}

void hold_predecessor_request(sintra::instance_id_type target)
{
    if (target != s_outbound_hold.expected.load(std::memory_order_acquire)) {
        return;
    }

    s_outbound_hold.entered.store(true, std::memory_order_release);
    while (!s_outbound_hold.release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
    }
}

#ifndef _WIN32
void observe_reap(pid_t pid, int status) noexcept
{
    if (pid == s_reaps.pid_0.load(std::memory_order_acquire)) {
        s_reaps.status_0.store(status, std::memory_order_relaxed);
        s_reaps.count_0.fetch_add(1, std::memory_order_release);
    }
    else if (pid == s_reaps.pid_1.load(std::memory_order_acquire)) {
        s_reaps.status_1.store(status, std::memory_order_relaxed);
        s_reaps.count_1.fetch_add(1, std::memory_order_release);
    }
}

bool signal_exact_process(int pid, uint64_t start_stamp, int signal_number)
{
    return exact_process_is_live(pid, start_stamp) &&
        ::kill(static_cast<pid_t>(pid), signal_number) == 0;
}
#endif

int recovery_child()
{
    const sintra::test::Shared_directory shared("SINTRA_R6_DIR", "managed_child_r6");
    const auto occurrence = sintra::s_recovery_occurrence;
    const char* nonce_value = std::getenv(k_nonce_env.data());
    const std::string nonce = nonce_value ? nonce_value : "";

    sintra::enable_recovery();
    const auto fixed_witness_iid = sintra::compose_instance(
        sintra::get_process_index(sintra::s_mproc_id),
        k_witness_instance_index);
    Recovery_witness witness(fixed_witness_iid);
    const bool named = witness.assign_name(std::string(k_witness_name));
    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();

    std::ostringstream record;
    record << "nonce=" << nonce << '\n'
           << "piid=" << static_cast<unsigned long long>(sintra::s_mproc_id) << '\n'
           << "witness_iid=" << static_cast<unsigned long long>(witness.instance_id()) << '\n'
           << "occurrence=" << occurrence << '\n'
           << "pid=" << pid << '\n'
           << "start_stamp_available=" << (start_stamp ? 1 : 0) << '\n'
           << "start_stamp=" << (start_stamp ? *start_stamp : 0) << '\n'
           << "witness_name=" << k_witness_name << '\n'
           << "complete=1\n";

    const auto ledger_name = occurrence == 0 ? k_ledger_0 : k_ledger_1;
    if (!named || nonce.empty() || occurrence > 1 ||
        !write_complete_file(marker(shared.path(), ledger_name), record.str()))
    {
        return 2;
    }

    if (occurrence == 0) {
        if (!sintra::test::wait_for_file(
                marker(shared.path(), k_crash_0), k_watchdog_timeout, 10ms))
        {
            return 2;
        }
        sintra::disable_debug_pause_for_current_process();
        sintra::test::prepare_for_intentional_crash("R6 occurrence 0");
        std::abort();
    }

    if (!sintra::test::wait_for_file(
            marker(shared.path(), k_release_1), k_watchdog_timeout, 10ms))
    {
        return 2;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_R6_DIR", "managed_child_r6");
    const std::string nonce =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
        std::to_string(sintra::test::get_pid());

    if (!is_spawned) {
#ifdef _WIN32
        _putenv_s(k_nonce_env.data(), nonce.c_str());
#else
        setenv(k_nonce_env.data(), nonce.c_str(), 1);
#endif
    }

    sintra::init(argc, argv, recovery_child);

    if (is_spawned) {
        const auto occurrence = sintra::s_recovery_occurrence;
        sintra::detail::finalize();
        if (occurrence == 1) {
            write_complete_file(marker(shared.path(), k_finalized_1), "complete=1\n");
        }
        return 0;
    }

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&]() {
        const auto deadline = std::chrono::steady_clock::now() + k_watchdog_timeout;
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "R6_INVALID watchdog_timeout=1\n");
                std::fflush(stderr);
                std::_Exit(2);
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    const bool ledger_0_seen = sintra::test::wait_for_file(
        marker(shared.path(), k_ledger_0), k_watchdog_timeout, 10ms);
    const auto ledger_0 = ledger_0_seen
        ? read_ledger(marker(shared.path(), k_ledger_0))
        : std::nullopt;

    bool ledger_0_valid = false;
    bool predecessor_live = false;
#ifdef _WIN32
    HANDLE process_0 = nullptr;
    HANDLE process_1 = nullptr;
#endif
    if (ledger_0) {
        const auto observed_stamp = sintra::query_process_start_stamp(
            static_cast<uint32_t>(ledger_0->pid));
        ledger_0_valid =
            ledger_0->nonce == nonce && ledger_0->occurrence == 0 &&
            ledger_0->witness_name == k_witness_name &&
            sintra::process_of(ledger_0->witness_iid) == ledger_0->process_iid &&
            ledger_0->witness_iid == sintra::compose_instance(
                sintra::get_process_index(ledger_0->process_iid), k_witness_instance_index) &&
            ledger_0->start_stamp_available && observed_stamp &&
            *observed_stamp == ledger_0->start_stamp &&
            resolve_publicly(ledger_0->witness_name) ==
                std::optional<sintra::instance_id_type>(ledger_0->witness_iid);
#ifdef _WIN32
        process_0 = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE, static_cast<DWORD>(ledger_0->pid));
        predecessor_live = process_0 && WaitForSingleObject(process_0, 0) == WAIT_TIMEOUT;
#else
        s_reaps.pid_0.store(static_cast<pid_t>(ledger_0->pid), std::memory_order_release);
        predecessor_live = exact_process_is_live(ledger_0->pid, ledger_0->start_stamp);
#endif
    }

#ifndef _WIN32
    sintra::test::managed_child::Scoped_test_hook child_reaped_hook(
        sintra::detail::test_hooks::s_child_reaped,
        &observe_reap);
#endif

    std::mutex recovery_mutex;
    std::optional<sintra::Crash_info> crash_info;
    std::optional<sintra::Recovery_control> recovery_control;
    std::atomic<bool> recovery_held{false};
    std::atomic<bool> recovery_released{false};
    sintra::set_recovery_runner([&](
        const sintra::Crash_info& info,
        const sintra::Recovery_control& control)
    {
        {
            std::lock_guard<std::mutex> lock(recovery_mutex);
            crash_info = info;
            recovery_control = control;
        }
        recovery_held.store(true, std::memory_order_release);
        while (!recovery_released.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        control.spawn();
    });

    s_outbound_hold.expected.store(
        ledger_0 ? ledger_0->witness_iid : sintra::invalid_instance_id,
        std::memory_order_release);
    s_outbound_hold.entered.store(false, std::memory_order_relaxed);
    s_outbound_hold.release.store(false, std::memory_order_relaxed);
    sintra::test::managed_child::Scoped_test_hook outbound_request_hook(
        sintra::detail::test_hooks::s_rpc_outbound_request,
        &hold_predecessor_request);

    constexpr uint32_t request_token = 0x5a31u;
    std::atomic<bool> request_done{false};
    std::atomic<bool> request_threw{false};
    std::atomic<uint32_t> request_result{0};
    std::thread predecessor_request([&]() {
        try {
            request_result.store(
                Recovery_witness::rpc_identify_occurrence(
                    std::string(k_witness_name),
                    request_token),
                std::memory_order_relaxed);
        }
        catch (...) {
            request_threw.store(true, std::memory_order_relaxed);
        }
        request_done.store(true, std::memory_order_release);
    });

    const bool predecessor_request_held = wait_until([&]() {
        return s_outbound_hold.entered.load(std::memory_order_acquire) &&
            !request_done.load(std::memory_order_acquire);
    }, 5s);

    const bool crash_requested = write_complete_file(
        marker(shared.path(), k_crash_0), "complete=1\n");
    const bool recovery_observed = wait_until([&]() {
        return recovery_held.load(std::memory_order_acquire);
    }, 10s);

    bool exact_crash = false;
    {
        std::lock_guard<std::mutex> lock(recovery_mutex);
        exact_crash = ledger_0 && crash_info &&
            crash_info->process_iid == ledger_0->process_iid &&
            crash_info->status != 0 && recovery_control &&
            !recovery_control->should_cancel();
    }
    const bool predecessor_retired = ledger_0 && recovery_observed &&
        resolve_publicly(ledger_0->witness_name) ==
            std::optional<sintra::instance_id_type>(sintra::invalid_instance_id);

    recovery_released.store(true, std::memory_order_release);
    const bool ledger_1_seen = sintra::test::wait_for_file(
        marker(shared.path(), k_ledger_1), 10s, 10ms);
    const auto ledger_1 = ledger_1_seen
        ? read_ledger(marker(shared.path(), k_ledger_1))
        : std::nullopt;
    bool ledger_1_valid = false;
    bool replacement_live = false;
    if (ledger_0 && ledger_1) {
        const auto observed_stamp = sintra::query_process_start_stamp(
            static_cast<uint32_t>(ledger_1->pid));
        ledger_1_valid =
            ledger_1->nonce == nonce && ledger_1->occurrence == ledger_0->occurrence + 1 &&
            ledger_1->process_iid == ledger_0->process_iid &&
            ledger_1->witness_iid == ledger_0->witness_iid &&
            ledger_1->witness_name == ledger_0->witness_name &&
            ledger_1->pid != ledger_0->pid &&
            ledger_1->start_stamp_available && observed_stamp &&
            *observed_stamp == ledger_1->start_stamp &&
            ledger_1->start_stamp != ledger_0->start_stamp &&
            resolve_publicly(ledger_1->witness_name) ==
                std::optional<sintra::instance_id_type>(ledger_1->witness_iid);
#ifdef _WIN32
        process_1 = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE, static_cast<DWORD>(ledger_1->pid));
        replacement_live = process_1 && WaitForSingleObject(process_1, 0) == WAIT_TIMEOUT;
#else
        s_reaps.pid_1.store(static_cast<pid_t>(ledger_1->pid), std::memory_order_release);
        replacement_live = exact_process_is_live(ledger_1->pid, ledger_1->start_stamp);
#endif
    }

    Custody_tuple_facts tuple_facts;
    std::shared_ptr<sintra::detail::Managed_child_custody_record> custody_record;
    auto observe_custody_tuple = [&]() {
        Custody_tuple_facts facts;
        if (!ledger_0 || !ledger_1 || !sintra::s_mproc) {
            return facts;
        }

        {
            std::lock_guard<std::mutex> lock(
                sintra::s_mproc->m_child_custody_mutex);
            const auto it = sintra::s_mproc->m_child_custody_by_process.find(
                ledger_1->process_iid);
            if (it != sintra::s_mproc->m_child_custody_by_process.end()) {
                custody_record = it->second.custody.lock();
            }
        }
        facts.record_found = static_cast<bool>(custody_record);
        if (!custody_record) {
            return facts;
        }

        std::lock_guard<std::mutex> lock(custody_record->mutex);
        facts.occurrence_count_exact = custody_record->occurrences.size() == 2;
        if (!facts.occurrence_count_exact) {
            return facts;
        }

        const auto& predecessor = custody_record->occurrences[0];
        const auto& replacement = custody_record->occurrences[1];
        facts.predecessor_occurrence =
            predecessor.occurrence == ledger_0->occurrence;
        facts.predecessor_setup =
            predecessor.setup == sintra::detail::Managed_child_occurrence_record::
                setup_state::ownership_ready;
        facts.predecessor_pid = predecessor.os_pid == ledger_0->pid;
        facts.predecessor_stamp_available =
            predecessor.os_process_start_stamp_available;
        facts.predecessor_stamp =
            predecessor.os_process_start_stamp == ledger_0->start_stamp;
        facts.predecessor_publication_retired =
            predecessor.transport.publication_retired();
        facts.predecessor_communication_retired =
            predecessor.transport.retirement_terminal();
        facts.predecessor_exit_confirmed = predecessor.os_exit_confirmed;
        facts.replacement_occurrence =
            replacement.occurrence == ledger_1->occurrence;
        facts.replacement_setup =
            replacement.setup == sintra::detail::Managed_child_occurrence_record::
                setup_state::ownership_ready;
        facts.replacement_pid = replacement.os_pid == ledger_1->pid;
        facts.replacement_stamp_available =
            replacement.os_process_start_stamp_available;
        facts.replacement_stamp =
            replacement.os_process_start_stamp == ledger_1->start_stamp;
        facts.replacement_created = replacement.os_process_created;
        facts.replacement_not_exited = !replacement.os_exit_confirmed;
        return facts;
    };

    bool tuple_observed_while_replacement_live_and_request_held = false;
    const auto tuple_deadline = std::chrono::steady_clock::now() + 10s;
    while (true) {
        const bool request_held_now = predecessor_request_held &&
            !request_done.load(std::memory_order_acquire);
#ifdef _WIN32
        const bool replacement_live_now =
            process_1 && WaitForSingleObject(process_1, 0) == WAIT_TIMEOUT;
#else
        const bool replacement_live_now = ledger_1 &&
            exact_process_is_live(ledger_1->pid, ledger_1->start_stamp);
#endif
        tuple_facts = observe_custody_tuple();
        if (tuple_facts.complete() && request_held_now && replacement_live_now) {
            tuple_observed_while_replacement_live_and_request_held = true;
            break;
        }
        if (!request_held_now || !replacement_live_now) {
            break;
        }
        if (std::chrono::steady_clock::now() >= tuple_deadline) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }

    const bool custody_occurrences_isolated = tuple_facts.complete() &&
        tuple_observed_while_replacement_live_and_request_held;
    if (!custody_occurrences_isolated) {
        std::fprintf(stderr,
            "R6_TUPLE_INVALID record=%d count=%d "
            "pred_occurrence=%d pred_setup=%d pred_pid=%d "
            "pred_stamp_available=%d pred_stamp=%d pred_publication_retired=%d "
            "pred_communication_retired=%d pred_exit_confirmed=%d "
            "replacement_occurrence=%d replacement_setup=%d replacement_pid=%d "
            "replacement_stamp_available=%d replacement_stamp=%d "
            "replacement_created=%d replacement_not_exited=%d "
            "observed_while_live_and_held=%d\n",
            tuple_facts.record_found ? 1 : 0,
            tuple_facts.occurrence_count_exact ? 1 : 0,
            tuple_facts.predecessor_occurrence ? 1 : 0,
            tuple_facts.predecessor_setup ? 1 : 0,
            tuple_facts.predecessor_pid ? 1 : 0,
            tuple_facts.predecessor_stamp_available ? 1 : 0,
            tuple_facts.predecessor_stamp ? 1 : 0,
            tuple_facts.predecessor_publication_retired ? 1 : 0,
            tuple_facts.predecessor_communication_retired ? 1 : 0,
            tuple_facts.predecessor_exit_confirmed ? 1 : 0,
            tuple_facts.replacement_occurrence ? 1 : 0,
            tuple_facts.replacement_setup ? 1 : 0,
            tuple_facts.replacement_pid ? 1 : 0,
            tuple_facts.replacement_stamp_available ? 1 : 0,
            tuple_facts.replacement_stamp ? 1 : 0,
            tuple_facts.replacement_created ? 1 : 0,
            tuple_facts.replacement_not_exited ? 1 : 0,
            tuple_observed_while_replacement_live_and_request_held ? 1 : 0);
    }

    const bool predecessor_request_still_held =
        predecessor_request_held && !request_done.load(std::memory_order_acquire);
    s_outbound_hold.release.store(true, std::memory_order_release);
    const bool request_completed = wait_until([&]() {
        return request_done.load(std::memory_order_acquire);
    }, 5s);
    predecessor_request.join();

    const bool replacement_satisfied_predecessor = ledger_0 && ledger_1 &&
        request_completed && !request_threw.load(std::memory_order_acquire) &&
        request_result.load(std::memory_order_relaxed) ==
            ((ledger_1->occurrence << 16) ^ request_token);

    const bool causal_witness =
        ledger_0_valid && predecessor_live && predecessor_request_held && crash_requested &&
        recovery_observed && exact_crash && predecessor_retired &&
        ledger_1_valid && replacement_live && predecessor_request_still_held &&
        replacement_satisfied_predecessor && custody_occurrences_isolated;

    const bool release_written = write_complete_file(
        marker(shared.path(), k_release_1), "complete=1\n");
    const bool replacement_finalized = sintra::test::wait_for_file(
        marker(shared.path(), k_finalized_1), 10s, 10ms);
    const bool root_finalized = sintra::detail::finalize();

    bool predecessor_abnormal_exit = false;
    bool replacement_normal_exit = false;
    bool no_survivors = false;
    bool forced_cleanup = false;
#ifdef _WIN32
    if (process_0 && WaitForSingleObject(process_0, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        predecessor_abnormal_exit = GetExitCodeProcess(process_0, &code) && code != 0;
    }
    if (process_1 && WaitForSingleObject(process_1, 10000) == WAIT_OBJECT_0) {
        DWORD code = STILL_ACTIVE;
        replacement_normal_exit = GetExitCodeProcess(process_1, &code) && code == 0;
    }
    no_survivors = predecessor_abnormal_exit && replacement_normal_exit;
    if (!no_survivors && ledger_1 && process_1 &&
        WaitForSingleObject(process_1, 0) == WAIT_TIMEOUT)
    {
        forced_cleanup = TerminateProcess(process_1, 2) != 0;
        WaitForSingleObject(process_1, 2000);
    }
    if (process_0) CloseHandle(process_0);
    if (process_1) CloseHandle(process_1);
#else
    wait_until([&]() {
        return s_reaps.count_0.load(std::memory_order_acquire) == 1 &&
            s_reaps.count_1.load(std::memory_order_acquire) == 1;
    }, 5s);
    const auto status_0 = s_reaps.status_0.load(std::memory_order_relaxed);
    const auto status_1 = s_reaps.status_1.load(std::memory_order_relaxed);
    predecessor_abnormal_exit = s_reaps.count_0.load(std::memory_order_acquire) == 1 &&
        WIFSIGNALED(status_0);
    replacement_normal_exit = s_reaps.count_1.load(std::memory_order_acquire) == 1 &&
        WIFEXITED(status_1) && WEXITSTATUS(status_1) == 0;
    no_survivors = ledger_0 && ledger_1 && predecessor_abnormal_exit && replacement_normal_exit &&
        !exact_process_is_live(ledger_0->pid, ledger_0->start_stamp) &&
        !exact_process_is_live(ledger_1->pid, ledger_1->start_stamp);
    if (!no_survivors && ledger_1 && signal_exact_process(ledger_1->pid, ledger_1->start_stamp, SIGTERM)) {
        forced_cleanup = true;
    }
    child_reaped_hook.restore();
#endif

    outbound_request_hook.restore();
    s_outbound_hold.expected.store(sintra::invalid_instance_id, std::memory_order_release);
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    const bool cleanup_valid = release_written && replacement_finalized &&
        predecessor_abnormal_exit && replacement_normal_exit && no_survivors && !forced_cleanup &&
        root_finalized;
    bool custody_release_closed_all_occurrences = false;
    if (custody_record) {
        std::lock_guard<std::mutex> lock(custody_record->mutex);
        custody_release_closed_all_occurrences =
            custody_record->phase == sintra::detail::Custody_phase::released &&
            std::all_of(
                custody_record->occurrences.begin(), custody_record->occurrences.end(),
                [](const auto& occurrence) {
                    return occurrence.setup == sintra::detail::Managed_child_occurrence_record::
                            setup_state::no_child ||
                        (occurrence.setup == sintra::detail::Managed_child_occurrence_record::
                            setup_state::ownership_ready &&
                         occurrence.transport.fully_retired() &&
                         occurrence.os_exit_confirmed);
                });
    }

    sintra::test::managed_child::Occurrence_isolation_outcome typed_outcome;
    auto copy_identity = [](const Occurrence_ledger& source) {
        sintra::test::managed_child::Occurrence_isolation_identity destination;
        destination.nonce = source.nonce;
        destination.process_iid = source.process_iid;
        destination.witness_iid = source.witness_iid;
        destination.occurrence = source.occurrence;
        destination.pid = source.pid;
        destination.start_stamp_available = source.start_stamp_available;
        destination.start_stamp = source.start_stamp;
        destination.witness_name = source.witness_name;
        return destination;
    };
    if (ledger_0) typed_outcome.predecessor = copy_identity(*ledger_0);
    if (ledger_1) typed_outcome.replacement = copy_identity(*ledger_1);
    typed_outcome.ledger_0_valid = ledger_0_valid;
    typed_outcome.predecessor_live = predecessor_live;
    typed_outcome.predecessor_request_held = predecessor_request_held;
    typed_outcome.recovery_observed = recovery_observed;
    typed_outcome.exact_crash = exact_crash;
    typed_outcome.predecessor_name_retired = predecessor_retired;
    typed_outcome.predecessor_request_still_held = predecessor_request_still_held;
    typed_outcome.ledger_1_valid = ledger_1_valid;
    typed_outcome.replacement_live = replacement_live;
    typed_outcome.replacement_satisfied_predecessor =
        replacement_satisfied_predecessor;
    typed_outcome.request_completed = request_completed;
    typed_outcome.request_threw = request_threw.load(std::memory_order_acquire);
    typed_outcome.request_result = request_result.load(std::memory_order_acquire);
    typed_outcome.custody_record_found = tuple_facts.record_found;
    typed_outcome.occurrence_count_exact = tuple_facts.occurrence_count_exact;
    typed_outcome.predecessor_occurrence = tuple_facts.predecessor_occurrence;
    typed_outcome.predecessor_setup = tuple_facts.predecessor_setup;
    typed_outcome.predecessor_pid = tuple_facts.predecessor_pid;
    typed_outcome.predecessor_stamp_available =
        tuple_facts.predecessor_stamp_available;
    typed_outcome.predecessor_stamp = tuple_facts.predecessor_stamp;
    typed_outcome.predecessor_publication_retired =
        tuple_facts.predecessor_publication_retired;
    typed_outcome.predecessor_communication_retired =
        tuple_facts.predecessor_communication_retired;
    typed_outcome.predecessor_exit_confirmed =
        tuple_facts.predecessor_exit_confirmed;
    typed_outcome.replacement_occurrence = tuple_facts.replacement_occurrence;
    typed_outcome.replacement_setup = tuple_facts.replacement_setup;
    typed_outcome.replacement_pid = tuple_facts.replacement_pid;
    typed_outcome.replacement_stamp_available =
        tuple_facts.replacement_stamp_available;
    typed_outcome.replacement_stamp = tuple_facts.replacement_stamp;
    typed_outcome.replacement_created = tuple_facts.replacement_created;
    typed_outcome.replacement_not_exited = tuple_facts.replacement_not_exited;
    typed_outcome.release_marker_written = release_written;
    typed_outcome.replacement_finalize_marker = replacement_finalized;
    typed_outcome.root_finalized = root_finalized;
    typed_outcome.predecessor_abnormal_exit = predecessor_abnormal_exit;
    typed_outcome.replacement_normal_exit = replacement_normal_exit;
#ifdef _WIN32
    if (custody_record) {
        std::lock_guard<std::mutex> lock(custody_record->mutex);
        if (custody_record->occurrences.size() == 2) {
            typed_outcome.predecessor_communication_retired =
                custody_record->occurrences[0].transport.retirement_terminal();
            typed_outcome.predecessor_native_exit_observer_registered =
                custody_record->occurrences[0].os_exit_observer_registered;
            typed_outcome.replacement_native_exit_observer_registered =
                custody_record->occurrences[1].os_exit_observer_registered;
        }
    }
#else
    if (custody_record) {
        std::lock_guard<std::mutex> lock(custody_record->mutex);
        if (custody_record->occurrences.size() == 2) {
            typed_outcome.predecessor_communication_retired =
                custody_record->occurrences[0].transport.retirement_terminal();
        }
    }
    typed_outcome.predecessor_native_exit_observer_registered = true;
    typed_outcome.replacement_native_exit_observer_registered = true;
#endif
    typed_outcome.forced_cleanup = forced_cleanup;
    typed_outcome.survivors_absent = no_survivors;
    typed_outcome.custody_release_complete =
        custody_release_closed_all_occurrences;
    sintra::test::managed_child::emit_occurrence_isolation_outcome(typed_outcome);

    if (causal_witness && cleanup_valid && custody_release_closed_all_occurrences) {
        std::fprintf(stdout,
            "R6_GREEN_VALID nonce=%s occurrence_0=%u pid_0=%d stamp_0=%llu "
            "occurrence_1=%u pid_1=%d stamp_1=%llu reused_piid=%llu reused_iid=%llu "
            "name_based_request=1 predecessor_request_held=1 name_retired=1 "
            "raw_name_request_retargeted=1 custody_retargeted=0 "
            "custody_occurrences_isolated=1 recovery_closed_before_release=1 "
            "all_occurrences_terminal=1 predecessor_abnormal=1 "
            "replacement_normal=1 forced_cleanup=0 survivors=0\n",
            nonce.c_str(), ledger_0->occurrence, ledger_0->pid,
            static_cast<unsigned long long>(ledger_0->start_stamp),
            ledger_1->occurrence, ledger_1->pid,
            static_cast<unsigned long long>(ledger_1->start_stamp),
            static_cast<unsigned long long>(ledger_1->process_iid),
            static_cast<unsigned long long>(ledger_1->witness_iid));
        std::fflush(stdout);
        shared.cleanup();
        return 0;
    }

    std::fprintf(stderr,
        "R6_INVALID ledger0=%d predecessor_live=%d request_held=%d recovery=%d exact_crash=%d "
        "retired=%d still_held=%d ledger1=%d replacement_live=%d replacement_satisfied=%d custody_isolated=%d "
        "release=%d finalized=%d predecessor_abnormal=%d replacement_normal=%d "
        "forced_cleanup=%d survivors_absent=%d custody_release_complete=%d\n",
        ledger_0_valid ? 1 : 0, predecessor_live ? 1 : 0, predecessor_request_held ? 1 : 0,
        recovery_observed ? 1 : 0, exact_crash ? 1 : 0, predecessor_retired ? 1 : 0,
        predecessor_request_still_held ? 1 : 0, ledger_1_valid ? 1 : 0,
        replacement_live ? 1 : 0, replacement_satisfied_predecessor ? 1 : 0,
        custody_occurrences_isolated ? 1 : 0,
        release_written ? 1 : 0, replacement_finalized ? 1 : 0,
        predecessor_abnormal_exit ? 1 : 0, replacement_normal_exit ? 1 : 0,
        forced_cleanup ? 1 : 0, no_survivors ? 1 : 0,
        custody_release_closed_all_occurrences ? 1 : 0);
    if (ledger_0 && ledger_1) {
        std::fprintf(stderr,
            "R6_INVALID_IDENTITIES piid0=%llu piid1=%llu iid0=%llu iid1=%llu "
            "occ0=%u occ1=%u pid0=%d pid1=%d stamp0=%llu stamp1=%llu "
            "request_result=%u request_threw=%d\n",
            static_cast<unsigned long long>(ledger_0->process_iid),
            static_cast<unsigned long long>(ledger_1->process_iid),
            static_cast<unsigned long long>(ledger_0->witness_iid),
            static_cast<unsigned long long>(ledger_1->witness_iid),
            ledger_0->occurrence, ledger_1->occurrence,
            ledger_0->pid, ledger_1->pid,
            static_cast<unsigned long long>(ledger_0->start_stamp),
            static_cast<unsigned long long>(ledger_1->start_stamp),
            request_result.load(std::memory_order_relaxed),
            request_threw.load(std::memory_order_relaxed) ? 1 : 0);
    }
    std::fflush(stderr);
    return 2;
}
