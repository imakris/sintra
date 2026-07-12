// Fail-first contract for exact predecessor communication retirement.

#include <sintra/sintra.h>
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
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace predecessor_retirement_test {

using Outcome = sintra::test::managed_child::Occurrence_isolation_outcome;

struct Gate
{
    std::mutex                                      mutex;
    std::condition_variable                         changed;
    std::shared_ptr<sintra::Process_message_reader> predecessor_reader;
    sintra::instance_id_type                        process_iid =
        sintra::invalid_instance_id;
    bool first_join_forced = false;
    bool first_join_incomplete = false;
    bool retirement_claim_reset = false;
    bool recovery_spawn_released = false;
    bool recovery_gate_timed_out = false;
    bool replacement_reader_installed = false;
    bool rpc_unblock_winner_claimed = false;
    bool rpc_unblock_follower_entered = false;
    bool rpc_unblock_follower_returned = false;
    bool rpc_unblock_follower_returned_after_completion = false;
    bool rpc_unblock_winner_released = false;
    bool rpc_unblock_follower_blocked_before_completion = false;
    bool rpc_unblock_complete_before_reader_setup = false;
    bool recovery_release_after_unblock_join_reset = false;
    unsigned rpc_unblock_entries = 0;
    unsigned rpc_unblock_completions = 0;
    sintra::instance_id_type rpc_unblock_process_iid =
        sintra::invalid_instance_id;
    bool recovery_info_observed = false;
    bool recovery_control_open = false;
    sintra::instance_id_type recovery_process_iid = sintra::invalid_instance_id;
    int recovery_status = 0;
};

inline Gate gate;
inline std::atomic<bool> forced_native_cleanup_called{false};
inline std::mutex outcome_mutex;
inline std::optional<Outcome> outcome;

inline void capture_outcome(const Outcome& observed)
{
    std::lock_guard<std::mutex> lock(outcome_mutex);
    outcome = observed;
}

inline bool wait_for_failed_claim_reset()
{
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (!gate.changed.wait_for(lock, std::chrono::seconds(5), [&]() {
                return gate.rpc_unblock_winner_claimed &&
                    gate.first_join_incomplete;
            }))
        {
            gate.recovery_gate_timed_out = true;
            gate.rpc_unblock_winner_released = true;
            gate.changed.notify_all();
            return false;
        }
        process_iid = gate.process_iid;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto token = sintra::s_mproc
            ? sintra::s_mproc->child_custody_occurrence_token(process_iid)
            : sintra::detail::Managed_child_occurrence_token{};
        if (auto custody = token.custody.lock()) {
            std::lock_guard<std::mutex> lock(custody->mutex);
            const auto exact = std::find_if(
                custody->occurrences.begin(), custody->occurrences.end(),
                [&](const auto& candidate) {
                    return candidate.process_instance_id == process_iid &&
                        candidate.occurrence == 0;
                });
            if (exact != custody->occurrences.end() &&
                exact->transport.ready_to_retire())
            {
                std::lock_guard<std::mutex> gate_lock(gate.mutex);
                gate.recovery_release_after_unblock_join_reset =
                    gate.rpc_unblock_completions == 1 &&
                    gate.rpc_unblock_process_iid == process_iid &&
                    gate.rpc_unblock_follower_returned_after_completion &&
                    gate.first_join_incomplete &&
                    !gate.recovery_spawn_released;
                if (!gate.recovery_release_after_unblock_join_reset) {
                    gate.recovery_gate_timed_out = true;
                    gate.changed.notify_all();
                    return false;
                }
                gate.retirement_claim_reset = true;
                gate.recovery_spawn_released = true;
                gate.changed.notify_all();
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.recovery_gate_timed_out = true;
    gate.changed.notify_all();
    return false;
}

inline void coordinate_rpc_unblock_race()
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    const bool replacement_started = gate.changed.wait_for(
        lock, std::chrono::seconds(20), [&]() {
            return gate.rpc_unblock_winner_claimed &&
                gate.predecessor_reader;
        });
    auto predecessor_reader = gate.predecessor_reader;
    lock.unlock();
    std::thread follower;
    if (replacement_started) {
        follower = std::thread([predecessor_reader]() {
            predecessor_reader->unblock_rpc_for_test();
            auto& gate = predecessor_retirement_test::gate;
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.rpc_unblock_follower_returned = true;
            gate.rpc_unblock_follower_returned_after_completion =
                gate.rpc_unblock_completions == 1 &&
                gate.rpc_unblock_process_iid == gate.process_iid;
            gate.changed.notify_all();
        });
    }
    lock.lock();
    const bool follower_waiting = replacement_started && gate.changed.wait_for(
        lock, std::chrono::seconds(5), [&]() {
            return gate.rpc_unblock_entries >= 2;
        });
    gate.rpc_unblock_follower_entered = follower_waiting;
    gate.rpc_unblock_follower_blocked_before_completion = follower_waiting &&
        gate.rpc_unblock_completions == 0 &&
        !gate.rpc_unblock_follower_returned &&
        !gate.replacement_reader_installed;
    if (!follower_waiting) {
        gate.recovery_gate_timed_out = true;
    }
    gate.rpc_unblock_winner_released = true;
    gate.changed.notify_all();
    lock.unlock();
    if (follower.joinable()) {
        follower.join();
    }
}

#ifdef _WIN32
inline BOOL tracked_terminate_process(HANDLE process, UINT exit_code)
{
    forced_native_cleanup_called.store(true, std::memory_order_release);
    return ::TerminateProcess(process, exit_code);
}
#else
inline int tracked_kill(pid_t pid, int signal_number)
{
    forced_native_cleanup_called.store(true, std::memory_order_release);
    return ::kill(pid, signal_number);
}
#endif

} // namespace predecessor_retirement_test

namespace sintra {

inline void set_recovery_runner_predecessor_gated(Recovery_runner runner)
{
    set_recovery_runner(
        [runner = std::move(runner)](
            const Crash_info& info,
            const Recovery_control& control)
        {
            {
                auto& gate = predecessor_retirement_test::gate;
                std::lock_guard<std::mutex> lock(gate.mutex);
                gate.recovery_info_observed = true;
                gate.recovery_process_iid = info.process_iid;
                gate.recovery_status = info.status;
                gate.recovery_control_open = !control.should_cancel();
                gate.changed.notify_all();
            }
            Recovery_control gated = control;
            gated.spawn = [spawn = control.spawn]() {
                if (predecessor_retirement_test::wait_for_failed_claim_reset()) {
                    spawn();
                }
            };
            runner(info, gated);
        });
}

} // namespace sintra

// Transform only the included baseline test; headers were included above.
#define set_recovery_runner set_recovery_runner_predecessor_gated
#ifdef _WIN32
#define TerminateProcess(...) \
    ::predecessor_retirement_test::tracked_terminate_process(__VA_ARGS__)
#else
#define kill(...) predecessor_retirement_test::tracked_kill(__VA_ARGS__)
#endif
#define main managed_child_occurrence_isolation_baseline_main
#include "managed_child_custody_occurrence_isolation_contract_test.cpp"
#undef main
#ifdef _WIN32
#undef TerminateProcess
#else
#undef kill
#endif
#undef set_recovery_runner

namespace {

using namespace std::chrono_literals;
using Outcome = predecessor_retirement_test::Outcome;

void force_predecessor_join_incomplete(
    const char*              stage,
    sintra::instance_id_type process_iid,
    uint32_t                 occurrence)
{
    if (!stage || occurrence != 0 || !sintra::s_mproc) return;
    auto& gate = predecessor_retirement_test::gate;
    const std::string_view observed(stage);
    if (observed ==
        sintra::detail::test_hooks::k_managed_child_communication_before_join)
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        if (gate.first_join_forced) return;
        if (!gate.predecessor_reader) return;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (gate.predecessor_reader->running_for_test() &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(10ms);
        }
        gate.predecessor_reader->set_running_for_test(true, false);
        gate.process_iid = process_iid;
        gate.first_join_forced = true;
    }
    else if (observed ==
        sintra::detail::test_hooks::k_managed_child_communication_join_incomplete)
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        if (gate.first_join_forced && gate.process_iid == process_iid) {
            gate.first_join_incomplete = true;
            gate.changed.notify_all();
        }
    }
}

void coordinate_reader_rpc_unblock(
    const char*              stage,
    sintra::instance_id_type process_iid,
    uint32_t                 occurrence)
{
    if (!stage || occurrence != 0) return;
    auto& gate = predecessor_retirement_test::gate;
    std::unique_lock<std::mutex> lock(gate.mutex);
    if (gate.process_iid == sintra::invalid_instance_id ||
        gate.process_iid != process_iid)
    {
        return;
    }
    const std::string_view observed(stage);
    if (observed ==
        sintra::detail::test_hooks::k_process_reader_rpc_unblock_entered)
    {
        ++gate.rpc_unblock_entries;
        gate.changed.notify_all();
        return;
    }
    if (observed ==
        sintra::detail::test_hooks::k_process_reader_rpc_unblock_claimed)
    {
        gate.rpc_unblock_winner_claimed = true;
        gate.changed.notify_all();
        if (!gate.changed.wait_for(lock, 5s, [&]() {
                return gate.rpc_unblock_winner_released;
            }))
        {
            gate.recovery_gate_timed_out = true;
            gate.rpc_unblock_winner_released = true;
            gate.changed.notify_all();
        }
        return;
    }
    if (observed ==
        sintra::detail::test_hooks::k_process_reader_rpc_unblock_complete)
    {
        ++gate.rpc_unblock_completions;
        gate.rpc_unblock_process_iid = process_iid;
        gate.rpc_unblock_complete_before_reader_setup =
            gate.rpc_unblock_completions == 1 &&
            !gate.replacement_reader_installed;
        gate.changed.notify_all();
    }
}

void observe_replacement_reader(
    sintra::instance_id_type process_iid,
    uint32_t                 occurrence)
{
    auto& gate = predecessor_retirement_test::gate;
    if (occurrence == 0) {
        const auto reader = sintra::s_mproc->m_readers.find(process_iid);
        if (reader == sintra::s_mproc->m_readers.end() || !reader->second ||
            reader->second->get_managed_child_custody_identity() == 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.predecessor_reader = reader->second;
        gate.process_iid = process_iid;
        gate.changed.notify_all();
        return;
    }
    if (occurrence != 1) return;
    // This hook is post-emplace. The winner/follower gate and the exact
    // predecessor reset gate above establish that IID cancellation completed
    // before recovery was released to reach this installation witness.
    std::lock_guard<std::mutex> lock(gate.mutex);
    if (!gate.first_join_forced || gate.process_iid != process_iid ||
        !gate.retirement_claim_reset || !gate.recovery_spawn_released ||
        !gate.rpc_unblock_follower_entered ||
        !gate.rpc_unblock_follower_blocked_before_completion ||
        !gate.rpc_unblock_complete_before_reader_setup ||
        gate.rpc_unblock_completions != 1 ||
        gate.rpc_unblock_process_iid != process_iid)
    {
        return;
    }
    gate.predecessor_reader->set_running_for_test(false, false);
    gate.replacement_reader_installed = true;
}

struct Exact_facts
{
    bool found = false;
    bool identities = false;
    bool records_match_outcome = false;
    bool setup = false;
    bool publication_retired = false;
    bool communication_started = false;
    bool communication_retired = false;
    bool predecessor_exit = false;
    bool replacement_terminal = false;
    bool active_replacement = false;
    bool release_complete = false;
    bool survivors_absent = false;
    bool predecessor_abnormal = false;
    bool replacement_normal = false;
    bool native_authority = false;
    bool observer_registered = false;
    uint64_t custody_identity = 0;
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    uint32_t predecessor_occurrence = 0;
    uint32_t replacement_occurrence = 0;
    int predecessor_pid = -1;
    int replacement_pid = -1;
    int predecessor_status = 0;
    int replacement_status = 0;
};

Exact_facts exact_facts(const Outcome& outcome)
{
    Exact_facts facts;
    if (!sintra::s_mproc) return facts;
    std::shared_ptr<sintra::detail::Managed_child_custody_record> custody;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
        for (const auto& [identity, candidate] : sintra::s_mproc->m_child_custodies) {
            if (candidate && candidate->occurrences.size() == 2) {
                custody = candidate;
                facts.custody_identity = identity;
                break;
            }
        }
    }
    if (!custody) return facts;

    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        const auto& predecessor = custody->occurrences[0];
        const auto& replacement = custody->occurrences[1];
        facts.found = true;
        facts.process_iid = predecessor.process_instance_id;
        facts.predecessor_occurrence = predecessor.occurrence;
        facts.replacement_occurrence = replacement.occurrence;
        facts.predecessor_pid = predecessor.os_pid;
        facts.replacement_pid = replacement.os_pid;
        facts.predecessor_status = predecessor.os_wait_status;
        facts.replacement_status = replacement.os_wait_status;
        facts.identities = predecessor.process_instance_id ==
                replacement.process_instance_id &&
            predecessor.occurrence == 0 && replacement.occurrence == 1 &&
            predecessor.os_pid > 0 && replacement.os_pid > 0 &&
            predecessor.os_pid != replacement.os_pid &&
            predecessor.os_process_start_stamp_available &&
            replacement.os_process_start_stamp_available &&
            predecessor.os_process_start_stamp != replacement.os_process_start_stamp;
        facts.records_match_outcome =
            outcome.predecessor.process_iid == predecessor.process_instance_id &&
            outcome.replacement.process_iid == replacement.process_instance_id &&
            outcome.predecessor.occurrence == predecessor.occurrence &&
            outcome.replacement.occurrence == replacement.occurrence &&
            outcome.predecessor.pid == predecessor.os_pid &&
            outcome.replacement.pid == replacement.os_pid &&
            outcome.predecessor.start_stamp == predecessor.os_process_start_stamp &&
            outcome.replacement.start_stamp == replacement.os_process_start_stamp;
        facts.setup = predecessor.setup == sintra::detail::
                Managed_child_occurrence_record::setup_state::ownership_ready &&
            replacement.setup == sintra::detail::
                Managed_child_occurrence_record::setup_state::ownership_ready &&
            predecessor.os_process_created && replacement.os_process_created &&
            !predecessor.initialization_reservation_active &&
            !replacement.initialization_reservation_active;
        facts.publication_retired =
            predecessor.transport.publication_retired();
        facts.communication_started =
            predecessor.transport.retirement_started();
        facts.communication_retired =
            predecessor.transport.retirement_terminal();
        facts.predecessor_exit = predecessor.os_exit_confirmed;
        facts.replacement_terminal = replacement.transport.fully_retired() &&
            replacement.os_exit_confirmed;
        facts.release_complete = custody->phase == sintra::detail::Custody_phase::released;
        facts.survivors_absent = facts.identities &&
            !sintra::test::managed_child::exact_process_is_live(
                predecessor.os_pid, predecessor.os_process_start_stamp) &&
            !sintra::test::managed_child::exact_process_is_live(
                replacement.os_pid, replacement.os_process_start_stamp);
#ifdef _WIN32
        facts.predecessor_abnormal = predecessor.os_wait_status_available &&
            predecessor.os_wait_status != 0;
        facts.replacement_normal = replacement.os_wait_status_available &&
            replacement.os_wait_status == 0;
        facts.native_authority = !predecessor.os_process_handle_owned &&
            !replacement.os_process_handle_owned &&
            predecessor.os_process_handle == 0 && replacement.os_process_handle == 0;
        facts.observer_registered = predecessor.os_exit_observer_registered &&
            replacement.os_exit_observer_registered;
#else
        facts.predecessor_abnormal = predecessor.os_wait_status_available &&
            WIFSIGNALED(predecessor.os_wait_status);
        facts.replacement_normal = replacement.os_wait_status_available &&
            WIFEXITED(replacement.os_wait_status) &&
            WEXITSTATUS(replacement.os_wait_status) == 0;
        facts.native_authority =
            s_reaps.pid_0.load() == predecessor.os_pid &&
            s_reaps.pid_1.load() == replacement.os_pid &&
            s_reaps.count_0.load() == 1 && s_reaps.count_1.load() == 1 &&
            s_reaps.status_0.load() == predecessor.os_wait_status &&
            s_reaps.status_1.load() == replacement.os_wait_status;
        facts.observer_registered = true;
#endif
    }
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
        const auto active = sintra::s_mproc->m_child_custody_by_process.find(
            facts.process_iid);
        facts.active_replacement =
            active != sintra::s_mproc->m_child_custody_by_process.end() &&
            active->second.custody.lock() == custody &&
            active->second.occurrence == facts.replacement_occurrence;
    }
    return facts;
}

bool unaffected_prefix(const Outcome& value, const Exact_facts& facts)
{
    constexpr uint32_t request_token = 0x5a31u;
    const auto expected_witness = sintra::compose_instance(
        sintra::get_process_index(facts.process_iid), k_witness_instance_index);
    const bool identities = !value.predecessor.nonce.empty() &&
        value.predecessor.nonce == value.replacement.nonce &&
        value.predecessor.process_iid == facts.process_iid &&
        value.replacement.process_iid == facts.process_iid &&
        value.predecessor.witness_iid == expected_witness &&
        value.replacement.witness_iid == expected_witness &&
        value.predecessor.witness_name == k_witness_name &&
        value.replacement.witness_name == k_witness_name &&
        value.predecessor.start_stamp_available &&
        value.replacement.start_stamp_available;
    const bool causal = value.ledger_0_valid && value.predecessor_live &&
        value.predecessor_request_held && value.recovery_observed &&
        value.exact_crash && value.predecessor_name_retired &&
        value.predecessor_request_still_held && value.ledger_1_valid &&
        value.replacement_live && value.replacement_satisfied_predecessor &&
        value.request_completed && !value.request_threw &&
        value.request_result == ((value.replacement.occurrence << 16) ^ request_token);
    const bool tuple = value.custody_record_found &&
        value.occurrence_count_exact && value.predecessor_occurrence &&
        value.predecessor_setup && value.predecessor_pid &&
        value.predecessor_stamp_available && value.predecessor_stamp &&
        value.predecessor_publication_retired &&
        !value.predecessor_communication_retired &&
        value.predecessor_exit_confirmed && value.replacement_occurrence &&
        value.replacement_setup && value.replacement_pid &&
        value.replacement_stamp_available && value.replacement_stamp &&
        value.replacement_created && value.replacement_not_exited;
    const bool cleanup = value.release_marker_written &&
        value.replacement_finalize_marker && !value.root_finalized &&
        value.predecessor_abnormal_exit && value.replacement_normal_exit &&
        !value.forced_cleanup && value.survivors_absent &&
        !value.custody_release_complete;
    return identities && causal && tuple && cleanup;
}

bool fixed_prefix(const Outcome& value)
{
    constexpr uint32_t request_token = 0x5a31u;
    const auto process_iid = value.predecessor.process_iid;
    const auto expected_witness = sintra::compose_instance(
        sintra::get_process_index(process_iid), k_witness_instance_index);
    const bool identities = !value.predecessor.nonce.empty() &&
        value.predecessor.nonce == value.replacement.nonce &&
        process_iid == value.replacement.process_iid &&
        value.predecessor.witness_iid == expected_witness &&
        value.replacement.witness_iid == expected_witness &&
        value.predecessor.witness_name == k_witness_name &&
        value.replacement.witness_name == k_witness_name &&
        value.predecessor.occurrence == 0 && value.replacement.occurrence == 1 &&
        value.predecessor.pid > 0 && value.replacement.pid > 0 &&
        value.predecessor.pid != value.replacement.pid &&
        value.predecessor.start_stamp_available &&
        value.replacement.start_stamp_available &&
        value.predecessor.start_stamp != value.replacement.start_stamp;
    const bool causal = value.ledger_0_valid && value.predecessor_live &&
        value.predecessor_request_held && value.recovery_observed &&
        value.exact_crash && value.predecessor_name_retired &&
        value.predecessor_request_still_held && value.ledger_1_valid &&
        value.replacement_live && value.replacement_satisfied_predecessor &&
        value.request_completed && !value.request_threw &&
        value.request_result == ((value.replacement.occurrence << 16) ^ request_token);
    const bool tuple = value.custody_record_found &&
        value.occurrence_count_exact && value.predecessor_occurrence &&
        value.predecessor_setup && value.predecessor_pid &&
        value.predecessor_stamp_available && value.predecessor_stamp &&
        value.predecessor_publication_retired &&
        value.predecessor_communication_retired &&
        value.predecessor_exit_confirmed && value.replacement_occurrence &&
        value.replacement_setup && value.replacement_pid &&
        value.replacement_stamp_available && value.replacement_stamp &&
        value.replacement_created && value.replacement_not_exited;
    const bool cleanup = value.release_marker_written &&
        value.replacement_finalize_marker && value.root_finalized &&
        value.predecessor_abnormal_exit && value.replacement_normal_exit &&
        value.predecessor_native_exit_observer_registered &&
        value.replacement_native_exit_observer_registered &&
        !value.forced_cleanup && value.survivors_absent &&
        value.custody_release_complete;
    return identities && causal && tuple && cleanup;
}

} // namespace

int main(int argc, char* argv[])
{
    if (sintra::test::has_branch_flag(argc, argv)) {
        return managed_child_occurrence_isolation_baseline_main(argc, argv);
    }
    sintra::test::managed_child::Scoped_test_hook outcome_hook(
        sintra::test::managed_child::s_occurrence_isolation_outcome,
        &predecessor_retirement_test::capture_outcome);
    sintra::test::managed_child::Scoped_test_hook transport_hook(
        sintra::detail::test_hooks::s_managed_child_transport_retirement,
        &force_predecessor_join_incomplete);
    sintra::test::managed_child::Scoped_test_hook reader_hook(
        sintra::detail::test_hooks::s_managed_child_reader_setup,
        &observe_replacement_reader);
    sintra::test::managed_child::Scoped_test_hook rpc_unblock_hook(
        sintra::detail::test_hooks::s_process_reader_rpc_unblock,
        &coordinate_reader_rpc_unblock);
    std::thread rpc_unblock_controller(
        &predecessor_retirement_test::coordinate_rpc_unblock_race);

    const int baseline_result =
        managed_child_occurrence_isolation_baseline_main(argc, argv);
    rpc_unblock_controller.join();
    const bool retry_finalized = sintra::detail::finalize();
    std::optional<Outcome> outcome;
    {
        std::lock_guard<std::mutex> lock(predecessor_retirement_test::outcome_mutex);
        outcome = predecessor_retirement_test::outcome;
    }
    if (!outcome) return 3;
    const auto facts = exact_facts(*outcome);

    auto& gate = predecessor_retirement_test::gate;
    bool ordering = false;
    bool recovery = false;
    bool replacement_reader = false;
    std::shared_ptr<sintra::Process_message_reader> predecessor_reader;
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        ordering = gate.first_join_forced && gate.first_join_incomplete &&
            gate.retirement_claim_reset && gate.recovery_spawn_released &&
            gate.rpc_unblock_winner_claimed &&
            gate.rpc_unblock_follower_entered &&
            gate.rpc_unblock_follower_returned &&
            gate.rpc_unblock_follower_returned_after_completion &&
            gate.rpc_unblock_winner_released &&
            gate.rpc_unblock_follower_blocked_before_completion &&
            gate.rpc_unblock_complete_before_reader_setup &&
            gate.recovery_release_after_unblock_join_reset &&
            gate.rpc_unblock_entries >= 2 &&
            gate.rpc_unblock_completions == 1 &&
            gate.rpc_unblock_process_iid == gate.process_iid &&
            !gate.recovery_gate_timed_out;
        recovery = gate.recovery_info_observed && gate.recovery_control_open &&
            gate.recovery_process_iid == outcome->predecessor.process_iid &&
            gate.recovery_status != 0;
        replacement_reader = gate.replacement_reader_installed;
        if (gate.predecessor_reader) {
            gate.predecessor_reader->set_running_for_test(false, false);
            predecessor_reader = std::move(gate.predecessor_reader);
        }
    }
    predecessor_reader.reset();

    const bool finalization_complete = outcome->root_finalized || retry_finalized;
    const bool exact_green = baseline_result == 2 && finalization_complete &&
        ordering && recovery && replacement_reader && fixed_prefix(*outcome) &&
        !predecessor_retirement_test::forced_native_cleanup_called.load();
    if (exact_green) {
        std::fprintf(stdout,
            "PREDECESSOR_RETIREMENT_GREEN piid=%llu occurrence_n=%u pid_n=%d "
            "occurrence_n1=%u pid_n1=%d first_join_incomplete=1 "
            "retirement_claim_reset=1 recovery_spawn_released=1 "
            "unblock_winner_claimed=1 unblock_follower_waited=1 "
            "unblock_follower_blocked_before_completion=1 iid_unblock_complete=1 "
            "follower_returned_after_iid_unblock=1 "
            "recovery_released_after_unblock_join_reset=1 "
            "replacement_reader_installed=1 full_typed_prefix=1 "
            "native_observer_registered=1 communication_n_retired=1 "
            "both_terminal=1 release_complete=1 finalization_complete=1 "
            "forced_cleanup=0 survivors=0\n",
            static_cast<unsigned long long>(outcome->predecessor.process_iid),
            outcome->predecessor.occurrence, outcome->predecessor.pid,
            outcome->replacement.occurrence, outcome->replacement.pid);
        return 0;
    }

    const bool exact_red = baseline_result == 2 && !retry_finalized &&
        ordering && recovery && replacement_reader && unaffected_prefix(*outcome, facts) &&
        facts.found && facts.identities && facts.records_match_outcome && facts.setup &&
        facts.publication_retired && !facts.communication_started &&
        !facts.communication_retired && facts.predecessor_exit &&
        facts.replacement_terminal && facts.active_replacement &&
        !facts.release_complete && facts.survivors_absent &&
        facts.predecessor_abnormal && facts.replacement_normal &&
        facts.native_authority && facts.observer_registered &&
        !predecessor_retirement_test::forced_native_cleanup_called.load();
    if (exact_red) {
        std::fprintf(stderr,
            "PREDECESSOR_RETIREMENT_RED custody=%llu piid=%llu occurrence_n=%u "
            "pid_n=%d occurrence_n1=%u pid_n1=%d first_join_incomplete=1 "
            "retirement_claim_reset=1 recovery_spawn_released=1 "
            "replacement_reader_installed=1 full_typed_prefix=1 "
            "records_bound=1 native_observer_registered=1 "
            "communication_n_retired=0 replacement_terminal=1 "
            "release_complete=0 retry_finalized=0 forced_cleanup=0 survivors=0\n",
            static_cast<unsigned long long>(facts.custody_identity),
            static_cast<unsigned long long>(facts.process_iid),
            facts.predecessor_occurrence, facts.predecessor_pid,
            facts.replacement_occurrence, facts.replacement_pid);
        return 86;
    }
    std::fprintf(stderr,
        "PREDECESSOR_RETIREMENT_INVALID baseline=%d retry=%d ordering=%d "
        "recovery=%d reader=%d typed_prefix=%d records=%d observer=%d "
        "native=%d survivors=%d winner=%d follower_entered=%d "
        "follower_returned=%d follower_after_complete=%d winner_released=%d "
        "blocked=%d complete_before=%d "
        "entries=%u completions=%u exact_iid=%d claim_reset=%d timeout=%d\n",
        baseline_result, retry_finalized ? 1 : 0, ordering ? 1 : 0,
        recovery ? 1 : 0, replacement_reader ? 1 : 0,
        unaffected_prefix(*outcome, facts) ? 1 : 0,
        facts.records_match_outcome ? 1 : 0,
        facts.observer_registered ? 1 : 0,
        facts.native_authority ? 1 : 0, facts.survivors_absent ? 1 : 0,
        gate.rpc_unblock_winner_claimed ? 1 : 0,
        gate.rpc_unblock_follower_entered ? 1 : 0,
        gate.rpc_unblock_follower_returned ? 1 : 0,
        gate.rpc_unblock_follower_returned_after_completion ? 1 : 0,
        gate.rpc_unblock_winner_released ? 1 : 0,
        gate.rpc_unblock_follower_blocked_before_completion ? 1 : 0,
        gate.rpc_unblock_complete_before_reader_setup ? 1 : 0,
        gate.rpc_unblock_entries, gate.rpc_unblock_completions,
        gate.rpc_unblock_process_iid == gate.process_iid ? 1 : 0,
        gate.retirement_claim_reset ? 1 : 0,
        gate.recovery_gate_timed_out ? 1 : 0);
    return 3;
}
