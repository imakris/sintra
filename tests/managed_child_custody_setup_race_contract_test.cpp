// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_environment.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

using namespace std::chrono_literals;
using sintra::test::managed_child::exact_process_is_absent;
using sintra::test::managed_child::process_identity_t;
using sintra::test::managed_child::read_process_identity;
using sintra::test::managed_child::wait_for_exact_process_absence;
using sintra::test::managed_child::write_process_identity;

constexpr const char* k_child_flag = "--managed-child-setup-race-child";
constexpr const char* k_native_bound_child_flag =
    "--managed-child-native-bound-failure-child";
constexpr const char* k_owned_child_flag = "--managed-child-owned-failure-child";
constexpr const char* k_hold_after_finalize_flag =
    "--managed-child-hold-after-finalize";
constexpr const char* k_prepublication_exit_child_flag =
    "--managed-child-prepublication-exit-child";
constexpr const char* k_immediate_exit_child_flag =
    "--managed-child-immediate-exit-child";
constexpr const char* k_readiness_identity_child_flag =
    "--managed-child-readiness-identity-child";
constexpr const char* k_readiness_cancellation_child_flag =
    "--managed-child-readiness-cancellation-child";
constexpr const char* k_post_native_recovery_child_flag =
    "--managed-child-post-native-recovery-child";
constexpr const char* k_recovery_recipe_exposure_child_flag =
    "--managed-child-recovery-recipe-exposure-child";

struct Unrelated_publication_target :
    sintra::Derived_transceiver<Unrelated_publication_target>
{};

struct Setup_gate
{
    std::mutex                   mutex;
    std::condition_variable      changed;
    sintra::instance_id_type     expected_iid = sintra::invalid_instance_id;
    uint32_t                     expected_occurrence = 0;
    bool                         entered = false;
    bool                         release = false;
};

Setup_gate* s_gate = nullptr;

struct Failure_plan
{
    std::mutex                  mutex;
    std::condition_variable     changed;
    const char*                 stage = nullptr;
    const char*                 park_stage = nullptr;
    sintra::instance_id_type    expected_iid = sintra::invalid_instance_id;
    uint32_t                    expected_occurrence = 0;
    unsigned                    remaining = 0;
    unsigned                    hits = 0;
    std::filesystem::path       wait_for_marker;
    bool                        marker_seen = false;
    bool                        parked = false;
    bool                        park_released = false;
    bool                        park_watchdog_released = false;
};

std::atomic<Failure_plan*> s_failure_plan{nullptr};

struct Prepublication_gate
{
    std::mutex                  mutex;
    std::condition_variable     changed;
    sintra::instance_id_type    expected_iid = sintra::invalid_instance_id;
    uint32_t                    expected_occurrence = 0;
    bool                        first_miss = false;
    bool                        publish_locked = false;
    bool                        reader_terminal = false;
    bool                        release_publish = false;
};

Prepublication_gate* s_prepublication_gate = nullptr;

struct Publication_identity_gate
{
    std::mutex              mutex;
    std::condition_variable changed;
    std::string             predecessor_name;
    std::string             replacement_name;
    bool                    predecessor_captured = false;
    bool                    release_predecessor = false;
    bool                    predecessor_retired = false;
    bool                    replacement_retired = false;
};

Publication_identity_gate* s_publication_identity_gate = nullptr;

bool wait_for_file(
    const std::filesystem::path&   path,
    std::chrono::milliseconds      timeout) noexcept;

struct Transport_retirement_gate
{
    std::mutex                  mutex;
    std::condition_variable     changed;
    sintra::instance_id_type    expected_iid = sintra::invalid_instance_id;
    uint32_t                    expected_occurrence = 0;
    bool                        before_join = false;
    bool                        join_incomplete = false;
    bool                        after_join = false;
    bool                        force_incomplete = false;
    std::chrono::steady_clock::time_point before_join_at{};
    std::chrono::steady_clock::time_point join_incomplete_at{};
    std::shared_ptr<sintra::Process_message_reader> reader;
};

Transport_retirement_gate* s_transport_retirement_gate = nullptr;

struct Release_worker_start_retry_gate
{
    std::mutex                  mutex;
    std::condition_variable     changed;
    sintra::instance_id_type    expected_iid = sintra::invalid_instance_id;
    bool                        unpublish_parked = false;
    bool                        release_unpublish = false;
    bool                        cleanup_parked = false;
    bool                        release_cleanup = false;
    bool                        communication_terminal_parked = false;
    bool                        release_communication = false;
    bool                        communication_resumed = false;
};

Release_worker_start_retry_gate* s_release_worker_start_retry_gate = nullptr;

void hold_release_worker_start_unpublish(const char* stage)
{
    auto* gate = s_release_worker_start_retry_gate;
    if (!gate || !stage || std::string_view(stage) !=
            sintra::detail::test_hooks::k_stage_unpublish_pre_barrier_collection)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->unpublish_parked = true;
    gate->changed.notify_all();
    gate->changed.wait_for(lock, 10s, [&]() {
        return gate->release_unpublish;
    });
}

void hold_release_worker_start_cleanup(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    auto* gate = s_release_worker_start_retry_gate;
    if (!gate || !stage || process_iid != gate->expected_iid ||
        occurrence != 0 || std::string_view(stage) !=
            sintra::detail::test_hooks::k_managed_child_cleanup_before_actions)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->cleanup_parked = true;
    gate->changed.notify_all();
    gate->changed.wait_for(lock, 10s, [&]() {
        return gate->release_cleanup;
    });
}

void hold_release_worker_start_communication(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    auto* gate = s_release_worker_start_retry_gate;
    if (!gate || !stage || process_iid != gate->expected_iid ||
        occurrence != 0 || std::string_view(stage) !=
            sintra::detail::test_hooks::
                k_managed_child_communication_terminal_before_reader_erase)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->communication_terminal_parked = true;
    gate->changed.notify_all();
    gate->changed.wait_for(lock, 10s, [&]() {
        return gate->release_communication;
    });
    gate->communication_resumed = true;
    gate->changed.notify_all();
}

#ifndef _WIN32
struct Roster_reservation_gate
{
    std::mutex                              mutex;
    std::condition_variable                 changed;
    std::array<sintra::instance_id_type, 2> expected_iids{};
    std::vector<uint64_t>                   reservations;
    bool                                    release = false;
};

Roster_reservation_gate* s_roster_gate = nullptr;

void hold_roster_reservation(
    sintra::instance_id_type process_iid,
    uint32_t,
    uint64_t reservation_id)
{
    auto* gate = s_roster_gate;
    if (!gate || (process_iid != gate->expected_iids[0] &&
        process_iid != gate->expected_iids[1]))
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->reservations.push_back(reservation_id);
    gate->changed.notify_all();
    gate->changed.wait(lock, [&]() { return gate->release; });
}
#endif

void observe_prepublication_cleanup(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    auto* gate = s_prepublication_gate;
    if (!gate || process_iid != gate->expected_iid ||
        occurrence != gate->expected_occurrence)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_prepublication_first_miss)
    {
        gate->first_miss = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&]() { return gate->publish_locked; });
    }
    else if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_prepublication_reader_terminal)
    {
        gate->reader_terminal = true;
        gate->changed.notify_all();
    }
}

void hold_transport_retirement(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    auto* gate = s_transport_retirement_gate;
    if (!gate || !stage || process_iid != gate->expected_iid ||
        occurrence != gate->expected_occurrence)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_communication_before_join)
    {
        if (gate->force_incomplete) {
            {
                sintra::Dispatch_shared_lock readers_lock(
                    sintra::s_mproc->m_readers_mutex);
                auto reader = sintra::s_mproc->m_readers.find(process_iid);
                if (reader != sintra::s_mproc->m_readers.end()) {
                    gate->reader = reader->second;
                }
            }
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (gate->reader && gate->reader->running_for_test() &&
                std::chrono::steady_clock::now() < deadline)
            {
                lock.unlock();
                std::this_thread::sleep_for(10ms);
                lock.lock();
            }
            if (gate->reader) {
                gate->reader->set_running_for_test(true, false);
            }
        }
        gate->before_join_at = std::chrono::steady_clock::now();
        gate->before_join = true;
        gate->changed.notify_all();
    }
    else if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_communication_join_incomplete)
    {
        gate->force_incomplete = false;
        gate->join_incomplete_at = std::chrono::steady_clock::now();
        gate->join_incomplete = true;
        gate->changed.notify_all();
    }
    else if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_communication_after_join)
    {
        gate->after_join = true;
        gate->changed.notify_all();
    }
}

void hold_inflight_publish(const char* stage)
{
    auto* gate = s_prepublication_gate;
    if (!gate || !stage || std::string_view(stage) !=
        sintra::detail::test_hooks::k_stage_publish_transceiver_locked)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->publish_locked = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&]() { return gate->release_publish; });
}

bool inject_managed_child_failure(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence) noexcept
{
    auto* plan = s_failure_plan.load(std::memory_order_acquire);
    if (!plan || !stage) {
        return false;
    }
    std::unique_lock<std::mutex> lock(plan->mutex);
    if (process_iid != plan->expected_iid ||
        occurrence != plan->expected_occurrence)
    {
        return false;
    }
    if (plan->stage && std::string_view(stage) == plan->stage &&
        !plan->wait_for_marker.empty())
    {
        plan->marker_seen = wait_for_file(plan->wait_for_marker, 5s);
    }
    if (plan->park_stage && std::string_view(stage) == plan->park_stage &&
        !plan->park_released)
    {
        plan->parked = true;
        plan->changed.notify_all();
        if (!plan->changed.wait_for(lock, 20s, [&]() {
                return plan->park_released;
            }))
        {
            plan->park_watchdog_released = true;
            plan->park_released = true;
            plan->changed.notify_all();
        }
        return false;
    }
    if (plan->remaining == 0 || std::string_view(stage) != plan->stage) {
        return false;
    }
    --plan->remaining;
    ++plan->hits;
    plan->changed.notify_all();
    return true;
}

unsigned failure_hits(Failure_plan& plan)
{
    std::lock_guard<std::mutex> lock(plan.mutex);
    return plan.hits;
}

#ifndef _WIN32
struct Posix_reap_observation
{
    std::atomic<pid_t> expected_pid{-1};
    std::atomic<unsigned> count{0};
    std::atomic<int> status{0};
};

Posix_reap_observation s_posix_reap;

struct Concurrent_posix_reap_observation
{
    std::array<std::atomic<pid_t>, 2> expected_pid{};
    std::array<std::atomic<unsigned>, 2> count{};
    std::array<std::atomic<int>, 2> status{};
};

Concurrent_posix_reap_observation s_concurrent_posix_reap;
std::atomic<pid_t> s_immediate_exit_pid{-1};
std::atomic<bool> s_immediate_exit_observed{false};

void wait_after_immediate_exec_handshake(pid_t pid)
{
    s_immediate_exit_pid.store(pid, std::memory_order_release);
    s_posix_reap.expected_pid.store(pid, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (sintra::is_process_alive(static_cast<uint32_t>(pid)) &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    s_immediate_exit_observed.store(
        !sintra::is_process_alive(static_cast<uint32_t>(pid)),
        std::memory_order_release);
}

void observe_posix_reap(pid_t pid, int status) noexcept
{
    if (pid != s_posix_reap.expected_pid.load(std::memory_order_acquire)) {
        return;
    }
    s_posix_reap.status.store(status, std::memory_order_relaxed);
    s_posix_reap.count.fetch_add(1, std::memory_order_release);
}

void observe_concurrent_posix_reap(pid_t pid, int status) noexcept
{
    for (size_t i = 0; i < 2; ++i) {
        if (pid == s_concurrent_posix_reap.expected_pid[i].load(
                std::memory_order_acquire))
        {
            s_concurrent_posix_reap.status[i].store(
                status, std::memory_order_relaxed);
            s_concurrent_posix_reap.count[i].fetch_add(
                1, std::memory_order_release);
            return;
        }
    }
}
#endif

void hold_reader_setup(sintra::instance_id_type process_iid, uint32_t occurrence)
{
    auto* gate = s_gate;
    if (!gate || process_iid != gate->expected_iid ||
        occurrence != gate->expected_occurrence)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->entered = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&]() { return gate->release; });
}

bool wait_for_gate(Setup_gate& gate)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.changed.wait_for(lock, 5s, [&]() { return gate.entered; });
}

void release_gate(Setup_gate& gate)
{
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.release = true;
    gate.changed.notify_all();
}

bool wait_for_release(
    const std::shared_ptr<sintra::detail::Managed_child_custody_record>& record)
{
    std::unique_lock<std::mutex> lock(record->mutex);
    return record->changed.wait_for(lock, 5s, [&]() {
        return record->release_state.released() &&
            record->readiness != sintra::detail::Readiness_phase::pending;
    });
}

std::filesystem::path unique_marker(const char* phase)
{
    return std::filesystem::temp_directory_path() /
        (std::string("sintra_custody_setup_race_") + phase + "_" +
         std::to_string(sintra::detail::get_current_process_id()) + ".marker");
}

bool marker_absent(const std::filesystem::path& marker)
{
    std::error_code ec;
    return !std::filesystem::exists(marker, ec);
}

std::filesystem::path release_marker(const std::filesystem::path& marker)
{
    return marker.string() + ".release";
}

std::filesystem::path occurrence_marker(
    const std::filesystem::path& marker,
    uint32_t occurrence)
{
    return marker.string() + ".occurrence." + std::to_string(occurrence);
}

std::filesystem::path finalized_marker(const std::filesystem::path& marker)
{
    return marker.string() + ".finalized";
}

std::filesystem::path exit_marker(const std::filesystem::path& marker)
{
    return marker.string() + ".exit";
}

bool wait_for_file(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::error_code ec;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path, ec)) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return std::filesystem::exists(path, ec);
}

bool write_release_marker(const std::filesystem::path& marker)
{
    std::ofstream out(release_marker(marker), std::ios::binary | std::ios::trunc);
    out << "release=1\n";
    return static_cast<bool>(out);
}

bool write_signal_marker(const std::filesystem::path& marker, const char* signal)
{
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    out << signal << "=1\n";
    return static_cast<bool>(out);
}

bool assigned_name_absent(const std::string& name)
{
    auto names = sintra::s_mproc->m_instance_id_of_assigned_name.scoped();
    return names.get().find(name) == names.get().end();
}

bool process_registry_absent(sintra::instance_id_type process_iid)
{
    std::lock_guard lock(sintra::s_coord->m_publish_mutex);
    return sintra::s_coord->m_transceiver_registry.find(process_iid) ==
        sintra::s_coord->m_transceiver_registry.end();
}

bool initialization_tracking_absent(sintra::instance_id_type process_iid)
{
    std::lock_guard lock(sintra::s_coord->m_init_tracking_mutex);
    return sintra::s_coord->m_processes_in_initialization.count(process_iid) == 0;
}

std::optional<std::array<bool, 3>> occurrence_terminal_facts(
    sintra::instance_id_type process_iid,
    uint32_t occurrence_number)
{
    const auto token = sintra::s_mproc->child_custody_occurrence_token(process_iid);
    auto custody = token.custody.lock();
    if (!custody || token.occurrence != occurrence_number) {
        return std::nullopt;
    }
    std::lock_guard lock(custody->mutex);
    auto occurrence = std::find_if(
        custody->occurrences.begin(),
        custody->occurrences.end(),
        [&](const sintra::detail::Managed_child_occurrence_record& candidate) {
            return candidate.process_instance_id == process_iid &&
                candidate.occurrence == occurrence_number;
        });
    if (occurrence == custody->occurrences.end()) {
        return std::nullopt;
    }
    return std::array{
        occurrence->initialization_reservation_active,
        occurrence->transport.publication_retired(),
        occurrence->transport.retirement_terminal()};
}

std::optional<std::array<bool, 4>> occurrence_release_attempt_facts(
    sintra::instance_id_type process_iid,
    uint32_t occurrence_number)
{
    const auto token = sintra::s_mproc->child_custody_occurrence_token(process_iid);
    auto custody = token.custody.lock();
    if (!custody || token.occurrence != occurrence_number) {
        return std::nullopt;
    }
    std::lock_guard lock(custody->mutex);
    auto occurrence = std::find_if(
        custody->occurrences.begin(),
        custody->occurrences.end(),
        [&](const sintra::detail::Managed_child_occurrence_record& candidate) {
            return candidate.process_instance_id == process_iid &&
                candidate.occurrence == occurrence_number;
        });
    if (occurrence == custody->occurrences.end()) {
        return std::nullopt;
    }
    return std::array{
        custody->release_state.active(),
        custody->release_state.failing() ||
            custody->release_state.retryable(),
        occurrence->transport.retirement_started(),
        custody->release_state.released()};
}

void reset_failure_hook()
{
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    s_failure_plan.store(nullptr, std::memory_order_release);
}

bool observed_setup_exception(
    const sintra::Managed_child_status& observation,
    const char* stage)
{
    return observation.last_failure.kind ==
            sintra::Managed_child_failure_kind::setup_exception &&
        observation.last_failure.occurrence == 0 &&
        observation.last_failure.native_error == 0 &&
        observation.last_failure.message.find(stage) != std::string::npos;
}

#ifndef _WIN32
void arm_posix_reap(const process_identity_t& identity)
{
    s_posix_reap.expected_pid.store(
        static_cast<pid_t>(identity.pid), std::memory_order_release);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &observe_posix_reap, std::memory_order_release);
}

bool posix_reap_normal()
{
    const auto count = s_posix_reap.count.load(std::memory_order_acquire);
    const auto status = s_posix_reap.status.load(std::memory_order_relaxed);
    return count == 1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool wait_for_exact_posix_reap(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = s_posix_reap.count.load(std::memory_order_acquire);
        if (count != 0) {
            return count == 1;
        }
        std::this_thread::sleep_for(10ms);
    }
    return s_posix_reap.count.load(std::memory_order_acquire) == 1;
}

void clear_posix_reap()
{
    sintra::detail::test_hooks::s_child_reaped.store(
        nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
}

void arm_concurrent_posix_reaps(
    const process_identity_t& first,
    const process_identity_t& second)
{
    const std::array<process_identity_t, 2> identities{first, second};
    for (size_t i = 0; i < 2; ++i) {
        s_concurrent_posix_reap.expected_pid[i].store(
            static_cast<pid_t>(identities[i].pid), std::memory_order_release);
        s_concurrent_posix_reap.count[i].store(0, std::memory_order_relaxed);
        s_concurrent_posix_reap.status[i].store(0, std::memory_order_relaxed);
    }
    sintra::detail::test_hooks::s_child_reaped.store(
        &observe_concurrent_posix_reap, std::memory_order_release);
}

bool concurrent_posix_reaps_normal()
{
    for (size_t i = 0; i < 2; ++i) {
        const auto count = s_concurrent_posix_reap.count[i].load(
            std::memory_order_acquire);
        const auto status = s_concurrent_posix_reap.status[i].load(
            std::memory_order_relaxed);
        if (count != 1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            return false;
        }
    }
    return true;
}

void clear_concurrent_posix_reaps()
{
    sintra::detail::test_hooks::s_child_reaped.store(
        nullptr, std::memory_order_release);
    for (auto& expected : s_concurrent_posix_reap.expected_pid) {
        expected.store(-1, std::memory_order_release);
    }
}
#endif

bool s_teardown_settled = true;

template <typename Finalizer>
bool settle_runtime_teardown(const char* phase, Finalizer&& finalizer)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    unsigned attempts = 0;
    do {
        ++attempts;
        try {
            if (finalizer()) {
                return true;
            }
        }
        catch (...) {
            s_teardown_settled = false;
            std::fprintf(stderr,
                "SETUP_FINALIZE_INVALID phase=%s exception=1 attempts=%u\n",
                phase, attempts);
            return false;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);

    s_teardown_settled = false;
    std::fprintf(stderr,
        "SETUP_FINALIZE_INVALID phase=%s settled=0 attempts=%u\n",
        phase, attempts);
    return false;
}

bool settle_detail_finalize(const char* phase)
{
    return settle_runtime_teardown(
        phase, []() { return sintra::detail::finalize(); });
}

bool run_recovery_create_release_race(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);

    const auto marker = unique_marker("recovery");
    std::error_code ec;
    std::filesystem::remove(marker, ec);

    auto custody = sintra::s_mproc->accept_child_custody();
    const auto piid = sintra::make_process_instance_id();
    Setup_gate gate;
    gate.expected_iid = piid;
    gate.expected_occurrence = 1;
    s_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        &hold_reader_setup, std::memory_order_release);

    sintra::Managed_process::Spawn_swarm_process_args args;
    args.binary_name = binary_path;
    args.args = {binary_path, k_child_flag, marker.string()};
    args.piid = piid;
    args.occurrence = 1;
    args.custody = custody;
    args.lifetime.enable_lifeline = false;

    sintra::Managed_process::Spawn_result result;
    std::thread create([&]() {
        auto launch_attempt = sintra::s_mproc->admit_child_custody_occurrence(
            custody, piid, args.occurrence);
        result = sintra::s_mproc->spawn_swarm_process(args, launch_attempt);
    });

    const bool setup_held = wait_for_gate(gate);
    bool production_cache_committed = false;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_cached_spawns_mutex);
        const auto cached = sintra::s_mproc->m_cached_spawns.find(piid);
        production_cache_committed =
            cached != sintra::s_mproc->m_cached_spawns.end() &&
            cached->second.piid == piid &&
            cached->second.custody == custody &&
            cached->second.occurrence == 2;
    }
    bool pending_before_release = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        pending_before_release = custody->occurrences.size() == 1 &&
            custody->occurrences.front().setup ==
                sintra::detail::Managed_child_occurrence_record::setup_state::pending;
    }

    sintra::s_mproc->request_child_custody_release(
        custody, sintra::detail::Release_mode::cleanup);
    std::this_thread::sleep_for(100ms);
    bool incomplete_while_held = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        incomplete_while_held =
            custody->release_state.releasing() &&
            custody->occurrences.front().setup ==
                sintra::detail::Managed_child_occurrence_record::setup_state::pending;
    }

    const bool no_child_while_held = marker_absent(marker);
    release_gate(gate);
    create.join();
    const bool release_completed = wait_for_release(custody);

    bool resolved_no_child = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        resolved_no_child = custody->occurrences.size() == 1 &&
            custody->occurrences.front().setup ==
                sintra::detail::Managed_child_occurrence_record::setup_state::no_child &&
            !custody->occurrences.front().native.created();
    }

    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        nullptr, std::memory_order_release);
    s_gate = nullptr;
    const bool registry_retired = sintra::s_mproc->wait_for_all_child_custodies(
        std::chrono::steady_clock::now() + 5s);
    bool cache_absent = false;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_cached_spawns_mutex);
        cache_absent =
            sintra::s_mproc->m_cached_spawns.find(piid) ==
                sintra::s_mproc->m_cached_spawns.end();
    }
    sintra::External_process_invitation_options invitation_options;
    invitation_options.process_instance_id = piid;
    invitation_options.timeout = 5s;
    const auto invitation = sintra::create_external_process_invitation(
        invitation_options);
    const bool invitation_created = static_cast<bool>(invitation);
    const bool invitation_cancelled = invitation_created &&
        sintra::cancel_external_process_invitation(invitation);
    const bool finalized = settle_detail_finalize("recovery_create_release");
    const bool no_child_after = marker_absent(marker);
    std::filesystem::remove(marker, ec);

    const bool valid = production_cache_committed && setup_held &&
        pending_before_release && incomplete_while_held &&
        no_child_while_held && !result.success && !result.os_process_created &&
        resolved_no_child && release_completed && registry_retired &&
        cache_absent && invitation_created && invitation_cancelled &&
        finalized && no_child_after;
    if (!valid) {
        std::fprintf(
            stderr,
            "RECOVERY_CACHE_RETIREMENT_INVALID production_cache=%d setup=%d "
            "pending=%d incomplete=%d no_child_held=%d spawn_success=%d "
            "os_created=%d resolved_no_child=%d release=%d registry=%d "
            "cache_absent=%d invitation=%d cancelled=%d finalized=%d "
            "no_child_after=%d\n",
            production_cache_committed ? 1 : 0,
            setup_held ? 1 : 0,
            pending_before_release ? 1 : 0,
            incomplete_while_held ? 1 : 0,
            no_child_while_held ? 1 : 0,
            result.success ? 1 : 0,
            result.os_process_created ? 1 : 0,
            resolved_no_child ? 1 : 0,
            release_completed ? 1 : 0,
            registry_retired ? 1 : 0,
            cache_absent ? 1 : 0,
            invitation_created ? 1 : 0,
            invitation_cancelled ? 1 : 0,
            finalized ? 1 : 0,
            no_child_after ? 1 : 0);
    }
    return valid;
}

bool run_recovery_recipe_exposure_race(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("recovery_recipe_exposure");
    const auto occurrence_0_marker = occurrence_marker(marker, 0);
    const auto occurrence_1_marker = occurrence_marker(marker, 1);
    const auto occurrence_0_crash = exit_marker(occurrence_0_marker);
    const auto occurrence_1_release = release_marker(occurrence_1_marker);
    const std::array paths{
        occurrence_0_marker,
        occurrence_1_marker,
        occurrence_0_crash,
        occurrence_1_release};
    std::error_code ec;
    for (const auto& path : paths) {
        std::filesystem::remove(path, ec);
        ec.clear();
    }

    const auto piid = sintra::make_process_instance_id();
    Failure_plan plan;
    plan.stage =
        sintra::detail::test_hooks::k_managed_child_fail_post_native_setup;
    plan.park_stage = plan.stage;
    plan.expected_iid = piid;
    plan.expected_occurrence = 0;
    plan.wait_for_marker = occurrence_0_marker;
    s_failure_plan.store(&plan, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    Setup_gate gate;
    gate.expected_iid = piid;
    gate.expected_occurrence = 1;
    s_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        &hold_reader_setup, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_recovery_recipe_exposure_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.lifetime.enable_lifeline = false;
    sintra::Managed_child_custody custody;
    std::thread initial_spawn([&]() {
        custody = sintra::spawn_swarm_process(options);
    });

    bool initial_parked = false;
    bool occurrence_0_initialized = false;
    {
        std::unique_lock<std::mutex> lock(plan.mutex);
        initial_parked = plan.changed.wait_for(lock, 10s, [&]() {
            return plan.parked;
        });
        occurrence_0_initialized = plan.marker_seen;
    }
    const bool occurrence_0_crashed = initial_parked &&
        occurrence_0_initialized &&
        write_signal_marker(occurrence_0_crash, "crash");
    const bool recovery_reader_entered = occurrence_0_crashed &&
        wait_for_gate(gate);

    bool parent_released_park = false;
    bool park_watchdog_released = false;
    {
        std::lock_guard<std::mutex> lock(plan.mutex);
        parent_released_park = plan.parked && !plan.park_released;
        plan.park_released = true;
        park_watchdog_released = plan.park_watchdog_released;
        plan.changed.notify_all();
    }
    initial_spawn.join();
    release_gate(gate);
    const bool occurrence_1_seen = recovery_reader_entered &&
        wait_for_file(occurrence_1_marker, 8s);
    const bool occurrence_1_released = occurrence_1_seen &&
        write_signal_marker(occurrence_1_release, "release");

    reset_failure_hook();
    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        nullptr, std::memory_order_release);
    s_gate = nullptr;
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 8s);
    const auto identity_0 = read_process_identity(occurrence_0_marker);
    const auto identity_1 = read_process_identity(occurrence_1_marker);
    const bool exact_distinct_children = identity_0 && identity_1 &&
        *identity_0 != *identity_1;
    const bool finalized = settle_detail_finalize(
        "recovery_recipe_exposure");
    for (const auto& path : paths) {
        std::filesystem::remove(path, ec);
        ec.clear();
    }

    const bool valid = custody && initial_parked &&
        occurrence_0_initialized && occurrence_0_crashed &&
        recovery_reader_entered && parent_released_park &&
        !park_watchdog_released && occurrence_1_seen &&
        occurrence_1_released && exact_distinct_children &&
        released.admitted_occurrences == 2 &&
        released.created_occurrences == 2 &&
        released.exited_occurrences == 2 &&
        released.release_state == sintra::Managed_child_release_state::complete &&
        finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "RECOVERY_RECIPE_EXPOSURE_INVALID custody=%d parked=%d marker0=%d "
            "crash0=%d reader1=%d parent_release=%d watchdog=%d marker1=%d "
            "release1=%d identity0=(%d,%llu) identity1=(%d,%llu) "
            "distinct=%d admitted=%zu created=%zu exited=%zu complete=%d "
            "finalized=%d\n",
            custody ? 1 : 0,
            initial_parked ? 1 : 0,
            occurrence_0_initialized ? 1 : 0,
            occurrence_0_crashed ? 1 : 0,
            recovery_reader_entered ? 1 : 0,
            parent_released_park ? 1 : 0,
            park_watchdog_released ? 1 : 0,
            occurrence_1_seen ? 1 : 0,
            occurrence_1_released ? 1 : 0,
            identity_0 ? identity_0->pid : -1,
            identity_0 ?
                static_cast<unsigned long long>(identity_0->start_stamp) : 0,
            identity_1 ? identity_1->pid : -1,
            identity_1 ?
                static_cast<unsigned long long>(identity_1->start_stamp) : 0,
            exact_distinct_children ? 1 : 0,
            released.admitted_occurrences,
            released.created_occurrences,
            released.exited_occurrences,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid;
}

bool run_deadline_setup_shutdown_retry(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);

    const auto marker = unique_marker("deadline");
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    const auto piid = sintra::make_process_instance_id();

    Setup_gate gate;
    gate.expected_iid = piid;
    gate.expected_occurrence = 0;
    s_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        &hold_reader_setup, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.readiness_instance_name = "managed_child_setup_race_never_published";
    options.lifetime.enable_lifeline = false;

    const auto started = std::chrono::steady_clock::now();
    auto custody = sintra::spawn_swarm_process(options);
    const auto observation = custody.wait_for_readiness_until(started + 200ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const bool setup_held = wait_for_gate(gate);
    const bool caller_bounded =
        elapsed >= 150ms && elapsed <= 1000ms && custody &&
        observation.admitted_occurrences == 1 &&
        observation.created_occurrences == 0 &&
        observation.readiness_state ==
            sintra::Managed_child_readiness_state::pending &&
        observation.last_failure.kind ==
            sintra::Managed_child_failure_kind::none &&
        observation.release_state == sintra::Managed_child_release_state::open;

    bool first_shutdown = true;
    bool second_shutdown = true;
    bool shutdown_threw = false;
    try {
        first_shutdown = sintra::shutdown();
        second_shutdown = sintra::shutdown();
    }
    catch (...) {
        shutdown_threw = true;
    }
    const bool retained_while_held =
        !first_shutdown && !second_shutdown && !shutdown_threw &&
        sintra::s_mproc != nullptr && marker_absent(marker);

    release_gate(gate);
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        nullptr, std::memory_order_release);
    s_gate = nullptr;

    const bool final_shutdown = settle_runtime_teardown(
        "deadline_setup_shutdown",
        []() { return sintra::shutdown(); });
    const bool no_child_after = marker_absent(marker);
    std::filesystem::remove(marker, ec);

    return setup_held && caller_bounded && retained_while_held &&
        released.release_state == sintra::Managed_child_release_state::complete &&
        final_shutdown && !shutdown_threw &&
        sintra::s_mproc == nullptr && no_child_after;
}

bool run_pre_create_exception(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("pre_create_exception");
    std::error_code ec;
    std::filesystem::remove(marker, ec);

    const auto piid = sintra::make_process_instance_id();
    Failure_plan plan;
    plan.stage = sintra::detail::test_hooks::k_managed_child_fail_pre_create_setup;
    plan.expected_iid = piid;
    plan.expected_occurrence = 0;
    plan.remaining = 1;
    s_failure_plan.store(&plan, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.readiness_instance_name = "managed_child_pre_create_never_published";
    options.lifetime.enable_lifeline = false;

    bool threw = false;
    sintra::Managed_child_custody custody;
    try {
        custody = sintra::spawn_swarm_process(options);
    }
    catch (...) {
        threw = true;
    }

    const auto hook_wait_started = std::chrono::steady_clock::now();
    bool hook_seen = false;
    {
        std::unique_lock<std::mutex> lock(plan.mutex);
        hook_seen = plan.changed.wait_for(
            lock, 5s, [&]() { return plan.hits == 1; });
    }
    const auto hook_wait_elapsed =
        std::chrono::steady_clock::now() - hook_wait_started;
    const bool hook_wait_bounded = hook_wait_elapsed < 6s;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);

    const auto readiness_started = std::chrono::steady_clock::now();
    try {
        if (custody) {
            custody.wait_for_readiness_until(readiness_started + 300ms);
        }
    }
    catch (...) {
        threw = true;
    }
    const auto readiness_elapsed =
        std::chrono::steady_clock::now() - readiness_started;
    const bool readiness_bounded = readiness_elapsed < 1500ms;

    const auto release_started = std::chrono::steady_clock::now();
    const auto released = custody.release_until(
        release_started + 5s);
    const auto release_elapsed =
        std::chrono::steady_clock::now() - release_started;
    const bool release_bounded = release_elapsed < 6s;
    const auto hits = failure_hits(plan);
    reset_failure_hook();
    const bool finalized = settle_detail_finalize("pre_create_exception");
    const bool marker_missing = marker_absent(marker);
    std::filesystem::remove(marker, ec);

    const bool failure_kind_match = released.last_failure.kind ==
        sintra::Managed_child_failure_kind::setup_exception;
    const bool failure_occurrence_match =
        released.last_failure.occurrence == 0;
    const bool failure_native_error_match =
        released.last_failure.native_error == 0;
    const bool failure_stage_match =
        released.last_failure.message.find(plan.stage) != std::string::npos;
    const bool valid = !threw && hook_seen && hook_wait_bounded &&
        readiness_bounded && release_bounded && custody &&
        released.admitted_occurrences == 1 &&
        released.created_occurrences == 0 && released.release_state ==
            sintra::Managed_child_release_state::complete && failure_kind_match &&
        failure_occurrence_match && failure_native_error_match &&
        failure_stage_match && observed_setup_exception(released, plan.stage) &&
        hits == 1 && marker_missing && finalized;
    if (!valid) {
        std::fprintf(stderr,
            "PRE_CREATE_EXCEPTION_INVALID threw=%d hook_seen=%d hits=%u "
            "hook_wait_bounded=%d readiness_bounded=%d release_bounded=%d "
            "accepted=%d admitted=%zu created=%zu release_requested=%d "
            "release_complete=%d failure_kind=%d kind_match=%d "
            "failure_occurrence=%u occurrence_match=%d native_error=%d "
            "native_error_match=%d stage_match=%d marker_missing=%d "
            "finalized=%d hook_wait_ms=%lld readiness_ms=%lld release_ms=%lld\n",
            threw ? 1 : 0,
            hook_seen ? 1 : 0,
            hits,
            hook_wait_bounded ? 1 : 0,
            readiness_bounded ? 1 : 0,
            release_bounded ? 1 : 0,
            custody ? 1 : 0,
            released.admitted_occurrences,
            released.created_occurrences,
            released.release_state !=
                sintra::Managed_child_release_state::open ? 1 : 0,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            static_cast<int>(released.last_failure.kind),
            failure_kind_match ? 1 : 0,
            released.last_failure.occurrence,
            failure_occurrence_match ? 1 : 0,
            released.last_failure.native_error,
            failure_native_error_match ? 1 : 0,
            failure_stage_match ? 1 : 0,
            marker_missing ? 1 : 0,
            finalized ? 1 : 0,
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    hook_wait_elapsed).count()),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    readiness_elapsed).count()),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    release_elapsed).count()));
    }
    return valid;
}

bool run_owned_native_exception(
    int argc,
    char* argv[],
    const std::string& binary_path,
    const char* phase,
    const char* failure_stage)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker(phase);
    const auto release = release_marker(marker);
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const auto piid = sintra::make_process_instance_id();
    Failure_plan plan;
    plan.stage = failure_stage;
    plan.expected_iid = piid;
    plan.expected_occurrence = 0;
    plan.remaining = 1;
    s_failure_plan.store(&plan, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_native_bound_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.readiness_instance_name =
        std::string("managed_child_") + phase + "_never_published";
    options.lifetime.enable_lifeline = false;

    const std::string unrelated_name =
        std::string("managed_child_unrelated_after_") + phase + "_" +
        std::to_string(piid);
    std::atomic<unsigned> unrelated_publications{0};
    auto observe_unrelated = [&](const sintra::Coordinator::instance_published& message) {
        if (static_cast<std::string>(message.assigned_name) == unrelated_name) {
            unrelated_publications.fetch_add(1, std::memory_order_release);
        }
    };
    sintra::activate_slot(
        observe_unrelated,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    bool threw = false;
    sintra::Managed_child_custody custody;
    try {
        custody = sintra::spawn_swarm_process(options);
        custody.wait_for_readiness_until(std::chrono::steady_clock::now() + 500ms);
    }
    catch (...) {
        threw = true;
    }
    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_process_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif
    const bool release_written = write_release_marker(marker);
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    const bool initialization_retired = initialization_tracking_absent(piid);
    bool unrelated_delivered = false;
    {
        Unrelated_publication_target unrelated;
        const bool assigned = unrelated.assign_name(unrelated_name);
        const auto publication_deadline = std::chrono::steady_clock::now() + 2s;
        while (unrelated_publications.load(std::memory_order_acquire) != 1 &&
            std::chrono::steady_clock::now() < publication_deadline)
        {
            std::this_thread::sleep_for(10ms);
        }
        unrelated_delivered = assigned &&
            unrelated_publications.load(std::memory_order_acquire) == 1;
    }
    sintra::deactivate_all_slots();
    const bool survivor_absent = identity &&
        wait_for_exact_process_absence(*identity, 5s);
    const auto hits = failure_hits(plan);
    reset_failure_hook();
#ifndef _WIN32
    const bool reap_seen = identity && wait_for_exact_posix_reap(5s);
    const bool reap_normal = reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_seen = true;
    const bool reap_normal = true;
#endif
    const bool finalized = settle_detail_finalize(phase);
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    return !threw && hits == 1 && identity && release_written &&
        custody && released.admitted_occurrences == 1 &&
        released.created_occurrences == 1 && released.exited_occurrences == 1 &&
        released.release_state == sintra::Managed_child_release_state::complete &&
        observed_setup_exception(released, failure_stage) &&
        initialization_retired && unrelated_delivered && survivor_absent &&
        reap_seen && reap_normal && finalized;
}

bool run_post_native_recovery_occurrence_advance(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("post_native_recovery_advance");
    const auto occurrence_0_marker = occurrence_marker(marker, 0);
    const auto occurrence_1_marker = occurrence_marker(marker, 1);
    const auto occurrence_2_marker = occurrence_marker(marker, 2);
    const auto occurrence_0_crash = exit_marker(occurrence_0_marker);
    const auto occurrence_1_crash = exit_marker(occurrence_1_marker);
    const auto occurrence_2_release = release_marker(occurrence_2_marker);
    const std::array paths{
        occurrence_0_marker,
        occurrence_1_marker,
        occurrence_2_marker,
        occurrence_0_crash,
        occurrence_1_crash,
        occurrence_2_release};
    std::error_code ec;
    for (const auto& path : paths) {
        std::filesystem::remove(path, ec);
        ec.clear();
    }

    const auto piid = sintra::make_process_instance_id();
    Failure_plan plan;
    plan.stage =
        sintra::detail::test_hooks::k_managed_child_fail_post_native_setup;
    plan.expected_iid = piid;
    plan.expected_occurrence = 1;
    plan.remaining = 1;
    plan.wait_for_marker = occurrence_1_marker;
    s_failure_plan.store(&plan, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_post_native_recovery_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    const bool occurrence_0_seen = wait_for_file(occurrence_0_marker, 5s);
    const bool occurrence_0_crashed = occurrence_0_seen &&
        write_signal_marker(occurrence_0_crash, "crash");
    bool hook_seen = false;
    bool occurrence_1_initialized = false;
    {
        std::unique_lock<std::mutex> lock(plan.mutex);
        hook_seen = plan.changed.wait_for(lock, 10s, [&]() {
            return plan.hits == 1;
        });
        occurrence_1_initialized = plan.marker_seen;
    }
    const bool occurrence_1_crashed = occurrence_1_initialized &&
        write_signal_marker(occurrence_1_crash, "crash");
    const bool occurrence_2_seen = occurrence_1_crashed &&
        wait_for_file(occurrence_2_marker, 8s);

    std::mutex exit_mutex;
    std::condition_variable exit_changed;
    sintra::Managed_child_exit exit_event;
    unsigned exit_count = 0;
    auto exit_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) {
            std::lock_guard<std::mutex> lock(exit_mutex);
            exit_event = event;
            ++exit_count;
            exit_changed.notify_all();
        });
    const bool occurrence_2_observed = exit_observation &&
        exit_observation.occurrence.process_instance_id == piid &&
        exit_observation.occurrence.occurrence == 2;
    const bool occurrence_2_released = occurrence_2_seen &&
        write_signal_marker(occurrence_2_release, "release");
    reset_failure_hook();
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 8s);
    bool occurrence_2_exit_observed = false;
    {
        std::unique_lock<std::mutex> lock(exit_mutex);
        occurrence_2_exit_observed = exit_changed.wait_for(
            lock, 5s, [&]() { return exit_count == 1; });
    }
    occurrence_2_exit_observed = occurrence_2_exit_observed &&
        exit_event.occurrence == exit_observation.occurrence;
    exit_observation.subscription.unsubscribe();

    const auto identity_0 = read_process_identity(occurrence_0_marker);
    const auto identity_1 = read_process_identity(occurrence_1_marker);
    const auto identity_2 = read_process_identity(occurrence_2_marker);
    const bool distinct_children = identity_0 && identity_1 && identity_2 &&
        *identity_0 != *identity_1 &&
        *identity_1 != *identity_2 &&
        *identity_0 != *identity_2;
    const bool setup_failure_retained =
        released.last_failure.kind ==
            sintra::Managed_child_failure_kind::setup_exception &&
        released.last_failure.occurrence == 1 &&
        released.last_failure.message.find(plan.stage) != std::string::npos;
    const bool finalized = settle_detail_finalize(
        "post_native_recovery_advance");
    for (const auto& path : paths) {
        std::filesystem::remove(path, ec);
        ec.clear();
    }

    const bool valid = custody && occurrence_0_seen && occurrence_0_crashed &&
        hook_seen && occurrence_1_initialized && occurrence_1_crashed &&
        occurrence_2_seen && occurrence_2_observed && occurrence_2_released &&
        occurrence_2_exit_observed && exit_count == 1 && distinct_children &&
        released.admitted_occurrences == 3 &&
        released.created_occurrences == 3 &&
        released.exited_occurrences == 3 &&
        released.release_state == sintra::Managed_child_release_state::complete &&
        setup_failure_retained && finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "POST_NATIVE_RECOVERY_ADVANCE_INVALID accepted=%d occurrence0=%d "
            "hook=%d marker1=%d crash1=%d occurrence2=%d observed2=%d "
            "released2=%d exit2=%d exit_count=%u observed_occurrence=%u "
            "admitted=%zu created=%zu exited=%zu release_complete=%d "
            "failure_kind=%d failure_occurrence=%u failure_stage=%d "
            "identity0=(%d,%llu) identity1=(%d,%llu) identity2=(%d,%llu) "
            "distinct=%d finalized=%d\n",
            custody ? 1 : 0,
            occurrence_0_seen ? 1 : 0,
            hook_seen ? 1 : 0,
            occurrence_1_initialized ? 1 : 0,
            occurrence_1_crashed ? 1 : 0,
            occurrence_2_seen ? 1 : 0,
            occurrence_2_observed ? 1 : 0,
            occurrence_2_released ? 1 : 0,
            occurrence_2_exit_observed ? 1 : 0,
            exit_count,
            exit_observation.occurrence.occurrence,
            released.admitted_occurrences,
            released.created_occurrences,
            released.exited_occurrences,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            static_cast<int>(released.last_failure.kind),
            released.last_failure.occurrence,
            setup_failure_retained ? 1 : 0,
            identity_0 ? identity_0->pid : -1,
            identity_0 ?
                static_cast<unsigned long long>(identity_0->start_stamp) : 0,
            identity_1 ? identity_1->pid : -1,
            identity_1 ?
                static_cast<unsigned long long>(identity_1->start_stamp) : 0,
            identity_2 ? identity_2->pid : -1,
            identity_2 ?
                static_cast<unsigned long long>(identity_2->start_stamp) : 0,
            distinct_children ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid;
}

enum class Release_worker_retry_result
{
    green,
    red_missing_report,
    invalid
};

const char* release_worker_retry_result_name(
    Release_worker_retry_result result)
{
    switch (result) {
        case Release_worker_retry_result::green:
            return "green";
        case Release_worker_retry_result::red_missing_report:
            return "red_missing_report";
        case Release_worker_retry_result::invalid:
            return "invalid";
    }
    return "invalid";
}

Release_worker_retry_result run_release_worker_retry(
    int argc,
    char* argv[],
    const std::string& binary_path,
    const char* phase,
    const char* failure_stage,
    sintra::Managed_child_failure_kind expected_failure_kind)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker(phase);
    const auto release = release_marker(marker);
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const bool force_terminal_capture_race =
        expected_failure_kind ==
            sintra::Managed_child_failure_kind::release_worker_start;
    const auto finalized_signal = finalized_marker(marker);
    const auto exit = exit_marker(marker);
    std::filesystem::remove(finalized_signal, ec);
    std::filesystem::remove(exit, ec);

    const auto piid = sintra::make_process_instance_id();
    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_owned_child_flag, marker.string()};
    if (force_terminal_capture_race) {
        options.args.push_back(k_hold_after_finalize_flag);
    }
    options.process_instance_id = piid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_process_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif

    Failure_plan plan;
    plan.stage = failure_stage;
    plan.expected_iid = piid;
    plan.expected_occurrence = 0;
    plan.remaining = 1;
    s_failure_plan.store(&plan, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    const auto first = custody.release_until(
        std::chrono::steady_clock::now() + 250ms);
    const auto hits_after_first = failure_hits(plan);
    bool release_written = false;
    bool retry_window_valid = !force_terminal_capture_race;
    sintra::Managed_child_status second;
    if (!force_terminal_capture_race) {
        release_written = write_release_marker(marker);
        second = custody.release_until(
            std::chrono::steady_clock::now() + 5s);
    }
    else {
        Release_worker_start_retry_gate gate;
        gate.expected_iid = piid;
        s_release_worker_start_retry_gate = &gate;
        sintra::detail::test_hooks::s_coordinator_lock_stage.store(
            &hold_release_worker_start_unpublish,
            std::memory_order_release);
        sintra::detail::test_hooks::s_managed_child_cleanup.store(
            &hold_release_worker_start_cleanup,
            std::memory_order_release);
        sintra::detail::test_hooks::s_managed_child_transport_retirement.store(
            &hold_release_worker_start_communication,
            std::memory_order_release);

        release_written = write_release_marker(marker);
        bool unpublish_parked = false;
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            unpublish_parked = gate.changed.wait_for(lock, 5s, [&]() {
                return gate.unpublish_parked;
            });
        }

        std::thread retry_caller;
        if (unpublish_parked) {
            retry_caller = std::thread([&]() {
                second = custody.terminate_until(
                    std::chrono::steady_clock::now() + 5s);
            });
        }

        bool cleanup_parked = false;
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            cleanup_parked = gate.changed.wait_for(lock, 5s, [&]() {
                return gate.cleanup_parked;
            });
            gate.release_unpublish = true;
            gate.changed.notify_all();
        }

        bool communication_terminal_parked = false;
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            communication_terminal_parked = gate.changed.wait_for(
                lock, 5s, [&]() {
                    return gate.communication_terminal_parked;
                });
        }
        const bool child_finalized = wait_for_file(finalized_signal, 5s);
        const bool exit_written = child_finalized &&
            write_signal_marker(exit, "exit");
        bool native_exit_confirmed = false;
        const auto exit_deadline = std::chrono::steady_clock::now() + 5s;
        do {
            native_exit_confirmed = custody.status().exited_occurrences == 1;
            if (!native_exit_confirmed) {
                std::this_thread::sleep_for(10ms);
            }
        } while (!native_exit_confirmed &&
            std::chrono::steady_clock::now() < exit_deadline);

        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.release_cleanup = true;
            gate.changed.notify_all();
        }
        if (retry_caller.joinable()) {
            retry_caller.join();
        }
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            gate.release_communication = true;
            gate.changed.notify_all();
            gate.changed.wait_for(lock, 5s, [&]() {
                return gate.communication_resumed;
            });
        }

        sintra::detail::test_hooks::s_coordinator_lock_stage.store(
            nullptr, std::memory_order_release);
        sintra::detail::test_hooks::s_managed_child_cleanup.store(
            nullptr, std::memory_order_release);
        sintra::detail::test_hooks::s_managed_child_transport_retirement.store(
            nullptr, std::memory_order_release);
        s_release_worker_start_retry_gate = nullptr;
        retry_window_valid = unpublish_parked && cleanup_parked &&
            communication_terminal_parked && child_finalized && exit_written &&
            native_exit_confirmed && gate.communication_resumed;

        if (second.release_state !=
            sintra::Managed_child_release_state::complete)
        {
            custody.terminate_until(std::chrono::steady_clock::now() + 5s);
        }
    }
    const bool survivor_absent = identity &&
        wait_for_exact_process_absence(*identity, 5s);
    reset_failure_hook();
#ifndef _WIN32
    const bool reap_seen = identity && wait_for_exact_posix_reap(5s);
    const bool reap_normal = reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_seen = true;
    const bool reap_normal = true;
#endif
    const bool finalized = settle_detail_finalize(phase);
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);
    std::filesystem::remove(finalized_signal, ec);
    std::filesystem::remove(exit, ec);

    const bool unaffected_prefix = identity && hits_after_first == 1 &&
        custody && retry_window_valid &&
        first.release_state == sintra::Managed_child_release_state::requested &&
        release_written &&
        second.release_state == sintra::Managed_child_release_state::complete &&
        second.created_occurrences == 1 &&
        second.exited_occurrences == 1 &&
        survivor_absent && reap_seen && reap_normal && finalized;

    const bool first_failure_is_typed =
        first.last_failure.kind == expected_failure_kind &&
        first.last_failure.occurrence == 0 &&
        first.last_failure.native_error == 0 &&
        first.last_failure.message.find(failure_stage) != std::string::npos;
    const bool historical_failure_is_exact =
        second.last_failure.kind == first.last_failure.kind &&
        second.last_failure.occurrence == first.last_failure.occurrence &&
        second.last_failure.native_error == first.last_failure.native_error &&
        second.last_failure.message == first.last_failure.message;
    if (unaffected_prefix && first_failure_is_typed &&
        historical_failure_is_exact)
    {
        return Release_worker_retry_result::green;
    }

    const bool report_missing =
        first.last_failure.kind == sintra::Managed_child_failure_kind::none &&
        second.last_failure.kind == sintra::Managed_child_failure_kind::none;
    return unaffected_prefix && report_missing
        ? Release_worker_retry_result::red_missing_report
        : Release_worker_retry_result::invalid;
}

bool run_prepublication_exit_convergence(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("prepublication_exit");
    std::error_code ec;
    std::filesystem::remove(marker, ec);

    const auto piid = sintra::make_process_instance_id();
    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_prepublication_exit_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.lifetime.enable_lifeline = false;

    auto custody = sintra::spawn_swarm_process(options);
    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_process_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif
    auto exited = custody.status();
    const auto exit_deadline = std::chrono::steady_clock::now() + 5s;
    while (exited.exited_occurrences != 1 &&
        std::chrono::steady_clock::now() < exit_deadline)
    {
        std::this_thread::sleep_for(10ms);
        exited = custody.status();
    }

    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    const bool initialization_retired = initialization_tracking_absent(piid);
    const bool survivor_absent = identity &&
        wait_for_exact_process_absence(*identity, 5s);
#ifndef _WIN32
    const bool reap_seen = identity && wait_for_exact_posix_reap(5s);
    const bool reap_normal = reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_seen = true;
    const bool reap_normal = true;
#endif
    const bool finalized = sintra::detail::finalize();
    std::filesystem::remove(marker, ec);

    const bool valid = identity && exited.created_occurrences == 1 &&
        exited.exited_occurrences == 1 && released.release_state ==
            sintra::Managed_child_release_state::complete &&
        released.admitted_occurrences == 1 &&
        released.created_occurrences == 1 && released.exited_occurrences == 1 &&
        initialization_retired && survivor_absent && reap_seen && reap_normal &&
        finalized;
    if (!valid) {
        std::fprintf(stderr,
            "PREPUBLICATION_EXIT_INVALID identity=%d created=%zu exited=%zu "
            "release_requested=%d release_complete=%d init_retired=%d "
            "survivor_absent=%d reap_seen=%d reap_normal=%d finalized=%d\n",
            identity ? 1 : 0,
            released.created_occurrences,
            released.exited_occurrences,
            released.release_state !=
                sintra::Managed_child_release_state::open ? 1 : 0,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            initialization_retired ? 1 : 0,
            survivor_absent ? 1 : 0,
            reap_seen ? 1 : 0,
            reap_normal ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid;
}

bool run_split_transport_retirement(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("split_transport_retirement");
    const auto release = release_marker(marker);
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const auto piid = sintra::make_process_instance_id();
    Transport_retirement_gate gate;
    gate.expected_iid = piid;
    gate.expected_occurrence = 0;
    gate.force_incomplete = true;
    s_transport_retirement_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_transport_retirement.store(
        &hold_transport_retirement, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_owned_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_process_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif

    sintra::Managed_child_status first_release;
    std::thread first_release_caller([&]() {
        first_release = custody.release_until(
            std::chrono::steady_clock::now() + 2500ms);
    });
    const bool release_written = write_release_marker(marker);
    bool join_incomplete = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        join_incomplete = gate.changed.wait_for(lock, 5s, [&]() {
            return gate.join_incomplete;
        });
    }
    if (first_release_caller.joinable()) {
        first_release_caller.join();
    }
    std::optional<std::array<bool, 4>> first_attempt;
    const auto first_attempt_deadline = std::chrono::steady_clock::now() + 2s;
    do {
        first_attempt = occurrence_release_attempt_facts(piid, 0);
        if (first_attempt && !(*first_attempt)[0]) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < first_attempt_deadline);
    const auto held_facts = occurrence_terminal_facts(piid, 0);
    const auto stop_elapsed = gate.join_incomplete_at - gate.before_join_at;
    const bool stop_deadline_bounded =
        stop_elapsed >= 900ms && stop_elapsed <= 1250ms;
    const bool first_pass_ended = join_incomplete && first_attempt &&
        !(*first_attempt)[0] && (*first_attempt)[1] &&
        !(*first_attempt)[2] && !(*first_attempt)[3] && held_facts &&
        !(*held_facts)[0] && (*held_facts)[1] && !(*held_facts)[2] &&
        first_release.release_state ==
            sintra::Managed_child_release_state::requested && stop_deadline_bounded;
    const bool first_blocker_reported =
        first_release.last_failure.kind ==
            sintra::Managed_child_failure_kind::release_worker_execution &&
        first_release.last_failure.occurrence == 0 &&
        first_release.last_failure.message.find("communication retirement") !=
            std::string::npos;

    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        if (gate.reader) {
            gate.reader->set_running_for_test(false, false);
        }
    }
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    bool after_join = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        after_join = gate.changed.wait_for(lock, 5s, [&]() {
            return gate.after_join;
        });
    }
    sintra::detail::test_hooks::s_managed_child_transport_retirement.store(
        nullptr, std::memory_order_release);
    s_transport_retirement_gate = nullptr;
    const bool survivor_absent = identity &&
        wait_for_exact_process_absence(*identity, 5s);
#ifndef _WIN32
    const bool reap_seen = identity && wait_for_exact_posix_reap(5s);
    const bool reap_normal = reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_seen = true;
    const bool reap_normal = true;
#endif

    const auto retry_marker = unique_marker("communication_worker_retry");
    const auto retry_release = release_marker(retry_marker);
    const auto retry_finalized = finalized_marker(retry_marker);
    const auto retry_exit = exit_marker(retry_marker);
    std::filesystem::remove(retry_marker, ec);
    std::filesystem::remove(retry_release, ec);
    std::filesystem::remove(retry_finalized, ec);
    std::filesystem::remove(retry_exit, ec);
    const auto retry_piid = sintra::make_process_instance_id();
    Failure_plan retry_plan;
    retry_plan.stage =
        sintra::detail::test_hooks::k_managed_child_fail_communication_worker_start;
    retry_plan.park_stage =
        sintra::detail::test_hooks::k_managed_child_fail_release_worker;
    retry_plan.expected_iid = retry_piid;
    retry_plan.expected_occurrence = 0;
    retry_plan.remaining = 1;
    s_failure_plan.store(&retry_plan, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    sintra::Spawn_options retry_options;
    retry_options.binary_path = binary_path;
    retry_options.args = {
        k_owned_child_flag,
        retry_marker.string(),
        k_hold_after_finalize_flag};
    retry_options.process_instance_id = retry_piid;
    retry_options.lifetime.enable_lifeline = false;
    auto retry_custody = sintra::spawn_swarm_process(retry_options);
    const bool retry_identity_written = wait_for_file(retry_marker, 5s);
    const auto retry_identity = retry_identity_written
        ? read_process_identity(retry_marker)
        : std::nullopt;
#ifndef _WIN32
    if (retry_identity) {
        arm_posix_reap(*retry_identity);
    }
#endif
    sintra::Managed_child_status retry_first_release;
    std::thread retry_first_caller([&]() {
        retry_first_release = retry_custody.release_until(
            std::chrono::steady_clock::now() + 2s);
    });

    bool worker_parked = false;
    {
        std::unique_lock<std::mutex> lock(retry_plan.mutex);
        worker_parked = retry_plan.changed.wait_for(lock, 5s, [&]() {
            return retry_plan.parked;
        });
    }
    std::optional<std::array<bool, 4>> retry_started_attempt;
    const auto retry_start_deadline = std::chrono::steady_clock::now() + 2s;
    do {
        retry_started_attempt = occurrence_release_attempt_facts(retry_piid, 0);
        if (retry_started_attempt && (*retry_started_attempt)[0] &&
            !(*retry_started_attempt)[1])
        {
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < retry_start_deadline);
    const bool worker_attempt_started = worker_parked && retry_started_attempt &&
        (*retry_started_attempt)[0] && !(*retry_started_attempt)[1];

    // Park the passive release worker before target selection, then let the
    // child finalize and unpublish. Coordinator retirement must claim the exact
    // reader and make the same release generation fail while that worker is
    // still parked. The child remains native-live after finalize so those facts
    // can be observed without an exit race.
    bool retry_release_written = worker_attempt_started &&
        write_release_marker(retry_marker);

    bool worker_failure_seen = false;
    if (worker_attempt_started && retry_release_written) {
        std::unique_lock<std::mutex> lock(retry_plan.mutex);
        worker_failure_seen = retry_plan.changed.wait_for(lock, 5s, [&]() {
            return retry_plan.hits == 1;
        });
    }

    std::optional<std::array<bool, 4>> retry_failing_attempt;
    const auto retry_failing_deadline = std::chrono::steady_clock::now() + 2s;
    do {
        retry_failing_attempt = occurrence_release_attempt_facts(retry_piid, 0);
        if (retry_failing_attempt && (*retry_failing_attempt)[0] &&
            (*retry_failing_attempt)[1])
        {
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < retry_failing_deadline);
    const bool worker_attempt_failing = worker_failure_seen &&
        retry_failing_attempt && (*retry_failing_attempt)[0] &&
        (*retry_failing_attempt)[1];
    const bool worker_finalize_completed = retry_release_written &&
        wait_for_file(retry_finalized, 10s);
    const auto retry_held_facts = occurrence_terminal_facts(retry_piid, 0);
    const bool worker_initialization_retired = retry_held_facts &&
        !(*retry_held_facts)[0];
    const bool worker_publication_retired = retry_held_facts &&
        (*retry_held_facts)[1];
    const bool worker_communication_nonterminal = retry_held_facts &&
        !(*retry_held_facts)[2];
    const bool worker_native_held_after_finalize =
        worker_finalize_completed && retry_identity &&
        !exact_process_is_absent(*retry_identity);

    bool worker_park_released = false;
    bool worker_park_watchdog_released = false;
    {
        std::lock_guard<std::mutex> lock(retry_plan.mutex);
        worker_park_released =
            retry_plan.parked && !retry_plan.park_released;
        retry_plan.park_released = true;
        worker_park_watchdog_released = retry_plan.park_watchdog_released;
        retry_plan.changed.notify_all();
    }

    // Neither the injected-failure latch nor the worker park may strand the
    // child. The callback also has a bounded watchdog release if this path
    // cannot perform the explicit release above.
    bool failure_hook_reset = false;
    bool retry_exit_written = false;
    if (!worker_failure_seen) {
        reset_failure_hook();
        failure_hook_reset = true;
        if (!retry_release_written) {
            retry_release_written = write_release_marker(retry_marker);
        }
        retry_exit_written = write_signal_marker(retry_exit, "exit");
    }
    if (retry_first_caller.joinable()) {
        retry_first_caller.join();
    }
    std::optional<std::array<bool, 4>> retry_first_attempt;
    const auto retry_attempt_deadline = std::chrono::steady_clock::now() + 2s;
    do {
        retry_first_attempt = occurrence_release_attempt_facts(retry_piid, 0);
        if (retry_first_attempt && !(*retry_first_attempt)[0]) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < retry_attempt_deadline);
    const bool worker_injection_exactly_once = worker_failure_seen &&
        failure_hits(retry_plan) == 1;
    const bool worker_attempt_stopped = retry_first_attempt &&
        !(*retry_first_attempt)[0];
    const bool worker_attempt_retryable = retry_first_attempt &&
        (*retry_first_attempt)[1];
    const bool worker_transport_not_started = retry_first_attempt &&
        !(*retry_first_attempt)[2];
    const bool worker_first_release_incomplete = retry_first_attempt &&
        !(*retry_first_attempt)[3] && retry_first_release.release_state ==
            sintra::Managed_child_release_state::requested;
    const bool worker_first_pass_ended =
        worker_attempt_started && worker_parked && worker_attempt_failing &&
        worker_park_released && !worker_park_watchdog_released &&
        worker_injection_exactly_once && worker_attempt_stopped &&
        worker_attempt_retryable && worker_transport_not_started &&
        worker_first_release_incomplete && worker_initialization_retired &&
        worker_publication_retired && worker_communication_nonterminal &&
        worker_native_held_after_finalize;

    if (!failure_hook_reset) {
        reset_failure_hook();
        failure_hook_reset = true;
    }
    if (!retry_release_written) {
        retry_release_written = write_release_marker(retry_marker);
    }
    if (!retry_exit_written) {
        retry_exit_written = write_signal_marker(retry_exit, "exit");
    }
    const bool retry_survivor_absent = retry_identity &&
        wait_for_exact_process_absence(*retry_identity, 10s);
    const auto retried = retry_custody.release_until(
        std::chrono::steady_clock::now() + 5s);
#ifndef _WIN32
    const bool worker_reap_seen = retry_identity &&
        wait_for_exact_posix_reap(5s);
    const bool retry_reap_normal = worker_reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool worker_reap_seen = true;
    const bool retry_reap_normal = true;
#endif
    const bool worker_retry_completed =
        retried.release_state == sintra::Managed_child_release_state::complete &&
        retried.created_occurrences == 1 && retried.exited_occurrences == 1;
    const bool worker_retry_valid = retry_identity && retry_release_written &&
        retry_exit_written &&
        worker_first_pass_ended && worker_retry_completed &&
        retry_survivor_absent && worker_reap_seen && retry_reap_normal;

    const bool finalized = settle_detail_finalize("split_transport_retirement");
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);
    std::filesystem::remove(retry_marker, ec);
    std::filesystem::remove(retry_release, ec);
    std::filesystem::remove(retry_finalized, ec);
    std::filesystem::remove(retry_exit, ec);

    const bool valid = identity && release_written && first_pass_ended &&
        first_blocker_reported && after_join &&
        released.release_state == sintra::Managed_child_release_state::complete &&
        released.created_occurrences == 1 &&
        released.exited_occurrences == 1 && survivor_absent && reap_seen &&
        reap_normal && worker_retry_valid && finalized;
    if (!valid) {
        std::fprintf(stderr,
            "SPLIT_TRANSPORT_INVALID identity=%d release_written=%d first_pass_ended=%d "
            "stop_deadline_bounded=%d blocker_reported=%d blocker_kind=%d "
            "blocker_occurrence=%u blocker_message='%s' after_join=%d "
            "release_complete=%d created=%zu exited=%zu "
            "survivor_absent=%d reap_seen=%d reap_normal=%d worker_retry=%d "
            "worker_identity=%d worker_finalize_release_written=%d "
            "worker_exit_release_written=%d worker_attempt_started=%d "
            "worker_parked=%d worker_attempt_failing=%d "
            "worker_park_released=%d worker_park_watchdog_released=%d "
            "worker_injection=%d "
            "worker_attempt_stopped=%d worker_attempt_retryable=%d "
            "worker_transport_not_started=%d worker_first_release_incomplete=%d "
            "worker_initialization_retired=%d worker_publication_retired=%d "
            "worker_communication_nonterminal=%d worker_finalize_completed=%d "
            "worker_native_held_after_finalize=%d "
            "worker_survivor_absent=%d worker_reap_seen=%d "
            "worker_reap_normal=%d "
            "worker_retry_complete=%d worker_created=%zu worker_exited=%zu "
            "finalized=%d\n",
            identity ? 1 : 0,
            release_written ? 1 : 0,
            first_pass_ended ? 1 : 0,
            stop_deadline_bounded ? 1 : 0,
            first_blocker_reported ? 1 : 0,
            static_cast<int>(first_release.last_failure.kind),
            first_release.last_failure.occurrence,
            first_release.last_failure.message.c_str(),
            after_join ? 1 : 0,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            released.created_occurrences,
            released.exited_occurrences,
            survivor_absent ? 1 : 0,
            reap_seen ? 1 : 0,
            reap_normal ? 1 : 0,
            worker_retry_valid ? 1 : 0,
            retry_identity ? 1 : 0,
            retry_release_written ? 1 : 0,
            retry_exit_written ? 1 : 0,
            worker_attempt_started ? 1 : 0,
            worker_parked ? 1 : 0,
            worker_attempt_failing ? 1 : 0,
            worker_park_released ? 1 : 0,
            worker_park_watchdog_released ? 1 : 0,
            worker_injection_exactly_once ? 1 : 0,
            worker_attempt_stopped ? 1 : 0,
            worker_attempt_retryable ? 1 : 0,
            worker_transport_not_started ? 1 : 0,
            worker_first_release_incomplete ? 1 : 0,
            worker_initialization_retired ? 1 : 0,
            worker_publication_retired ? 1 : 0,
            worker_communication_nonterminal ? 1 : 0,
            worker_finalize_completed ? 1 : 0,
            worker_native_held_after_finalize ? 1 : 0,
            retry_survivor_absent ? 1 : 0,
            worker_reap_seen ? 1 : 0,
            retry_reap_normal ? 1 : 0,
            worker_retry_completed ? 1 : 0,
            retried.created_occurrences,
            retried.exited_occurrences,
            finalized ? 1 : 0);
    }
    return valid;
}

bool run_prepublication_publish_race(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("prepublication_publish_race");
    const auto release = release_marker(marker);
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const auto piid = sintra::make_process_instance_id();
    const std::string published_name =
        "managed_child_inflight_publish_" + std::to_string(piid);

    Failure_plan plan;
    plan.stage = sintra::detail::test_hooks::k_managed_child_fail_post_native_setup;
    plan.expected_iid = piid;
    plan.expected_occurrence = 0;
    plan.remaining = 1;
    s_failure_plan.store(&plan, std::memory_order_release);

    Prepublication_gate gate;
    gate.expected_iid = piid;
    gate.expected_occurrence = 0;
    s_prepublication_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_prepublication_cleanup.store(
        &observe_prepublication_cleanup, std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &hold_inflight_publish, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_native_bound_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.readiness_instance_name = "managed_child_prepublication_never_published";
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    custody.wait_for_readiness_until(std::chrono::steady_clock::now() + 500ms);

    bool first_miss = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        first_miss = gate.changed.wait_for(lock, 5s, [&]() {
            return gate.first_miss;
        });
    }

    auto exit_observation = custody.observe_latest_created_exit(
        [](const sintra::Managed_child_exit&) {});
    const bool observation_valid = static_cast<bool>(exit_observation);
    const auto publication_identity = exit_observation.occurrence;
    exit_observation.subscription.unsubscribe();
    const bool publication_identity_exact = observation_valid &&
        publication_identity.custody_identity != 0 &&
        publication_identity.process_instance_id == piid &&
        publication_identity.occurrence == 0;

    sintra::instance_id_type publish_result = sintra::invalid_instance_id;
    std::thread publisher([&]() {
        publish_result =
            sintra::s_coord->publish_managed_child_transceiver_for_test(
                sintra::make_user_type_id(1001),
                piid,
                published_name,
                publication_identity.custody_identity,
                publication_identity.process_instance_id,
                publication_identity.occurrence);
    });

    bool reader_terminal = false;
    bool publish_held = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        reader_terminal = gate.changed.wait_for(lock, 5s, [&]() {
            return gate.reader_terminal;
        });
        publish_held = gate.publish_locked && !gate.release_publish;
    }
    const auto held_observation = custody.status();

    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_process_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif
    const bool release_written = write_release_marker(marker);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release_publish = true;
        gate.changed.notify_all();
    }
    publisher.join();

    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    const bool survivor_absent = identity &&
        wait_for_exact_process_absence(*identity, 5s);
    const bool canonical_absence =
        assigned_name_absent(published_name) && process_registry_absent(piid);

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_prepublication_cleanup.store(
        nullptr, std::memory_order_release);
    s_prepublication_gate = nullptr;
    reset_failure_hook();
#ifndef _WIN32
    const bool reap_seen = identity && wait_for_exact_posix_reap(5s);
    const bool reap_normal = reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_seen = true;
    const bool reap_normal = true;
#endif
    const bool finalized = settle_detail_finalize("prepublication_publish_race");
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const bool valid = first_miss && publication_identity_exact &&
        reader_terminal && publish_held &&
        held_observation.release_state ==
            sintra::Managed_child_release_state::requested && publish_result == piid &&
        release_written && released.release_state ==
            sintra::Managed_child_release_state::complete && canonical_absence &&
        survivor_absent && reap_seen && reap_normal && finalized;
    if (!valid) {
        std::fprintf(stderr,
            "PREPUBLICATION_INVALID first_miss=%d identity_exact=%d "
            "reader_terminal=%d publish_held=%d "
            "held_incomplete=%d publish_result=%d release_written=%d release_complete=%d "
            "canonical_absence=%d survivor_absent=%d reap_seen=%d reap_normal=%d "
            "finalized=%d\n",
            first_miss ? 1 : 0,
            publication_identity_exact ? 1 : 0,
            reader_terminal ? 1 : 0, publish_held ? 1 : 0,
            held_observation.release_state !=
                sintra::Managed_child_release_state::complete ? 1 : 0,
            publish_result == piid ? 1 : 0, release_written ? 1 : 0,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            canonical_absence ? 1 : 0,
            survivor_absent ? 1 : 0, reap_seen ? 1 : 0,
            reap_normal ? 1 : 0, finalized ? 1 : 0);
    }
    return valid;
}

void hold_predecessor_publication_identity(const char* stage)
{
    auto* gate = s_publication_identity_gate;
    if (!gate || !stage || std::string_view(stage) !=
        sintra::detail::test_hooks::k_stage_managed_child_publication_identity_captured)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    if (gate->predecessor_captured) {
        return;
    }
    gate->predecessor_captured = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&]() { return gate->release_predecessor; });
}

void observe_occurrence_publication_retirement(
    sintra::instance_id_type,
    const std::string& assigned_name)
{
    auto* gate = s_publication_identity_gate;
    if (!gate) {
        return;
    }
    std::lock_guard<std::mutex> lock(gate->mutex);
    if (assigned_name == gate->predecessor_name) {
        gate->predecessor_retired = true;
    }
    if (assigned_name == gate->replacement_name) {
        gate->replacement_retired = true;
    }
}

bool run_unrelated_readiness_rejection(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    static std::atomic<sintra::instance_id_type> expected_spawn_iid{
        sintra::invalid_instance_id};
    static std::atomic<int> observed_spawn_pid{-1};
    static std::atomic<unsigned> observed_spawn_count{0};
    auto observe_spawn_success = +[](
        sintra::instance_id_type process_iid,
        int                      os_pid,
        bool,
        bool)
    {
        if (process_iid != expected_spawn_iid.load(std::memory_order_acquire)) {
            return;
        }
        observed_spawn_pid.store(os_pid, std::memory_order_relaxed);
        observed_spawn_count.fetch_add(1, std::memory_order_release);
    };

    sintra::init(argc, argv);
    const auto marker = unique_marker("readiness_unrelated");
    const auto release = release_marker(marker);
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const auto piid = sintra::make_process_instance_id();
    const std::string target_name =
        "managed_child_readiness_unrelated_" + std::to_string(piid);

    bool unrelated_assigned = false;
    bool unrelated_resolved = false;
    bool exact_identity_rejected = false;
    bool spawn_observed = false;
    bool identity_written = false;
    bool start_stamp_matched = false;
    bool release_written = false;
    bool survivor_absent = false;
    bool reap_seen = false;
    bool reap_normal = false;
    sintra::Managed_child_status released;
    {
        Unrelated_publication_target unrelated;
        unrelated_assigned = unrelated.assign_name(target_name);
        const auto unrelated_iid = sintra::Coordinator::rpc_resolve_instance(
            sintra::s_coord_id, target_name);
        unrelated_resolved = unrelated_iid != sintra::invalid_instance_id &&
            sintra::process_of(unrelated_iid) == sintra::s_mproc_id;

        sintra::Spawn_options options;
        options.binary_path = binary_path;
        options.args = {k_native_bound_child_flag, marker.string()};
        options.process_instance_id = piid;
        options.lifetime.enable_lifeline = false;

        expected_spawn_iid.store(piid, std::memory_order_release);
        observed_spawn_pid.store(-1, std::memory_order_relaxed);
        observed_spawn_count.store(0, std::memory_order_relaxed);
        sintra::detail::test_hooks::s_runtime_spawn_success.store(
            observe_spawn_success, std::memory_order_release);
        auto custody = sintra::spawn_swarm_process(options);
        sintra::detail::test_hooks::s_runtime_spawn_success.store(
            nullptr, std::memory_order_release);
        expected_spawn_iid.store(
            sintra::invalid_instance_id, std::memory_order_release);

        const auto observed = custody.status();
        const auto identity = wait_for_file(marker, 5s)
            ? read_process_identity(marker)
            : std::nullopt;
        identity_written = identity.has_value();
        spawn_observed = identity && custody &&
            observed.admitted_occurrences == 1 &&
            observed.created_occurrences == 1 &&
            observed_spawn_count.load(std::memory_order_acquire) == 1 &&
            observed_spawn_pid.load(std::memory_order_relaxed) == identity->pid;
        if (identity) {
            const auto live_start_stamp = sintra::query_process_start_stamp(
                static_cast<uint32_t>(identity->pid));
            start_stamp_matched = live_start_stamp &&
                *live_start_stamp == identity->start_stamp;
        }

        const auto occurrence_token =
            sintra::s_mproc->child_custody_occurrence_token(piid);
        const auto occurrence_custody = occurrence_token.custody.lock();
        if (occurrence_custody &&
            occurrence_token.process_instance_id == piid)
        {
            const auto exact_resolution =
                sintra::detail::Managed_child_readiness_access::resolve(
                    sintra::s_coord,
                    target_name,
                    occurrence_custody->identity,
                    piid,
                    occurrence_token.occurrence,
                    occurrence_custody->readiness_cancelled);
            exact_identity_rejected =
                exact_resolution == sintra::invalid_instance_id;
        }
#ifndef _WIN32
        if (identity) {
            arm_posix_reap(*identity);
        }
#endif
        release_written = write_release_marker(marker);
        released = custody.release_until(
            std::chrono::steady_clock::now() + 5s);
        survivor_absent = identity &&
            wait_for_exact_process_absence(*identity, 5s);
#ifndef _WIN32
        reap_seen = identity && wait_for_exact_posix_reap(5s);
        reap_normal = reap_seen && posix_reap_normal();
        clear_posix_reap();
#else
        reap_seen = true;
        reap_normal = true;
#endif
    }

    const bool finalized = settle_detail_finalize("readiness_unrelated");
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const bool valid = unrelated_assigned && unrelated_resolved &&
        exact_identity_rejected && spawn_observed && identity_written &&
        start_stamp_matched && release_written && released.release_state ==
            sintra::Managed_child_release_state::complete &&
        released.created_occurrences == 1 && released.exited_occurrences == 1 &&
        survivor_absent && reap_seen && reap_normal && finalized;
    if (!valid) {
        std::fprintf(stderr,
            "READINESS_UNRELATED_INVALID assigned=%d resolved=%d exact_rejected=%d "
            "spawn_observed=%d identity=%d start_stamp=%d release_written=%d "
            "release_complete=%d created=%zu exited=%zu survivor_absent=%d "
            "reap_seen=%d reap_normal=%d finalized=%d\n",
            unrelated_assigned ? 1 : 0,
            unrelated_resolved ? 1 : 0,
            exact_identity_rejected ? 1 : 0,
            spawn_observed ? 1 : 0,
            identity_written ? 1 : 0,
            start_stamp_matched ? 1 : 0,
            release_written ? 1 : 0,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            released.created_occurrences,
            released.exited_occurrences,
            survivor_absent ? 1 : 0,
            reap_seen ? 1 : 0,
            reap_normal ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid;
}

bool run_exact_readiness_acceptance(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("readiness_exact");
    const auto release = release_marker(marker);
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const auto piid = sintra::make_process_instance_id();
    const std::string target_name =
        "managed_child_readiness_exact_" + std::to_string(piid);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        k_readiness_identity_child_flag,
        marker.string(),
        target_name};
    options.process_instance_id = piid;
    options.readiness_instance_name = target_name;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    const auto observed = custody.wait_for_readiness_until(
        std::chrono::steady_clock::now() + 5s);
    const auto resolved = sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id, target_name);
    const bool exact_publication = resolved != sintra::invalid_instance_id &&
        sintra::process_of(resolved) == piid;
    const auto identity = wait_for_file(marker, 5s)
        ? read_process_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif
    const bool release_written = write_release_marker(marker);
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    const bool survivor_absent = identity &&
        wait_for_exact_process_absence(*identity, 5s);
#ifndef _WIN32
    const bool reap_seen = identity && wait_for_exact_posix_reap(5s);
    const bool reap_normal = reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_seen = true;
    const bool reap_normal = true;
#endif
    const bool finalized = settle_detail_finalize("readiness_exact");
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const bool valid = custody && observed.readiness_state ==
            sintra::Managed_child_readiness_state::reached &&
        observed.created_occurrences == 1 && exact_publication && identity &&
        release_written && released.release_state ==
            sintra::Managed_child_release_state::complete &&
        released.created_occurrences == 1 && released.exited_occurrences == 1 &&
        survivor_absent && reap_seen && reap_normal && finalized;
    if (!valid) {
        std::fprintf(stderr,
            "READINESS_EXACT_INVALID accepted=%d readiness=%d created=%zu "
            "exact_publication=%d identity=%d release_written=%d "
            "release_complete=%d exited=%zu survivor_absent=%d reap_seen=%d "
            "reap_normal=%d finalized=%d\n",
            custody ? 1 : 0,
            observed.readiness_state ==
                sintra::Managed_child_readiness_state::reached ? 1 : 0,
            observed.created_occurrences,
            exact_publication ? 1 : 0,
            identity ? 1 : 0,
            release_written ? 1 : 0,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            released.exited_occurrences,
            survivor_absent ? 1 : 0,
            reap_seen ? 1 : 0,
            reap_normal ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid;
}

bool run_unbounded_readiness_cancellation(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("readiness_cancellation");
    std::error_code ec;
    std::filesystem::remove(marker, ec);

    const auto piid = sintra::make_process_instance_id();
    sintra::Managed_child_custody custody;
    std::atomic<bool> spawn_returned{false};
    std::atomic<bool> readiness_wait_returned{false};
    std::thread spawn_caller([&]() {
        sintra::Spawn_options options;
        options.binary_path = binary_path;
        options.args = {k_readiness_cancellation_child_flag, marker.string()};
        options.process_instance_id = piid;
        options.readiness_instance_name =
            "managed_child_readiness_cancel_never_" + std::to_string(piid);
        options.lifetime.enable_lifeline = false;
        custody = sintra::spawn_swarm_process(options);
        spawn_returned.store(true, std::memory_order_release);
        custody.wait_for_readiness_until(std::chrono::steady_clock::time_point::max());
        readiness_wait_returned.store(true, std::memory_order_release);
    });

    const auto identity = wait_for_file(marker, 5s)
        ? read_process_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif
    const bool blocked_before_finalize = identity &&
        spawn_returned.load(std::memory_order_acquire) &&
        !readiness_wait_returned.load(std::memory_order_acquire);
    std::atomic<bool> finalize_done{false};
    std::thread cancellation_watchdog([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (!finalize_done.load(std::memory_order_acquire) &&
            std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(20ms);
        }
        if (finalize_done.load(std::memory_order_acquire)) {
            return;
        }
        const bool caller_returned = spawn_returned.load(std::memory_order_acquire);
        const auto observed = caller_returned
            ? custody.status()
            : sintra::Managed_child_status{};
        const bool all_custodies_released = sintra::s_mproc &&
            sintra::s_mproc->all_child_custodies_released();
        const bool coordinator_sole = sintra::s_coord &&
            sintra::s_coord->is_sole_known_process(sintra::s_mproc_id);
        std::fprintf(stderr,
            "READINESS_CANCELLATION_STUCK spawn_returned=%d accepted=%d "
            "readiness=%d release_requested=%d release_complete=%d "
            "created=%zu exited=%zu all_custodies_released=%d "
            "coordinator_sole=%d runtime_present=%d coordinator_present=%d\n",
            caller_returned ? 1 : 0,
            custody ? 1 : 0,
            observed.readiness_state ==
                sintra::Managed_child_readiness_state::reached ? 1 : 0,
            observed.release_state !=
                sintra::Managed_child_release_state::open ? 1 : 0,
            observed.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            observed.created_occurrences,
            observed.exited_occurrences,
            all_custodies_released ? 1 : 0,
            coordinator_sole ? 1 : 0,
            sintra::s_mproc ? 1 : 0,
            sintra::s_coord ? 1 : 0);
        std::fflush(stderr);
        std::_Exit(2);
    });
    const auto finalize_started = std::chrono::steady_clock::now();
    const bool finalized = settle_detail_finalize("readiness_cancellation");
    const auto finalize_elapsed = std::chrono::steady_clock::now() - finalize_started;
    finalize_done.store(true, std::memory_order_release);
    cancellation_watchdog.join();
    spawn_caller.join();

    const auto released = custody.status();
    const bool survivor_absent = identity &&
        wait_for_exact_process_absence(*identity, 5s);
#ifndef _WIN32
    const bool reap_seen = identity && wait_for_exact_posix_reap(5s);
    const bool reap_normal = reap_seen && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_seen = true;
    const bool reap_normal = true;
#endif
    std::filesystem::remove(marker, ec);

    const bool valid = blocked_before_finalize && finalized &&
        finalize_elapsed < 5s && spawn_returned.load(std::memory_order_acquire) &&
        custody && released.readiness_state ==
            sintra::Managed_child_readiness_state::observation_stopped &&
        released.release_state == sintra::Managed_child_release_state::complete &&
        released.created_occurrences == 1 && released.exited_occurrences == 1 &&
        survivor_absent && reap_seen && reap_normal && sintra::s_mproc == nullptr;
    if (!valid) {
        std::fprintf(stderr,
            "READINESS_CANCELLATION_INVALID blocked=%d finalized=%d bounded=%d "
            "spawn_returned=%d accepted=%d readiness=%d release_requested=%d "
            "release_complete=%d created=%zu exited=%zu survivor_absent=%d "
            "reap_seen=%d reap_normal=%d runtime_gone=%d\n",
            blocked_before_finalize ? 1 : 0,
            finalized ? 1 : 0,
            finalize_elapsed < 5s ? 1 : 0,
            spawn_returned.load(std::memory_order_acquire) ? 1 : 0,
            custody ? 1 : 0,
            released.readiness_state ==
                sintra::Managed_child_readiness_state::reached ? 1 : 0,
            released.release_state !=
                sintra::Managed_child_release_state::open ? 1 : 0,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            released.created_occurrences,
            released.exited_occurrences,
            survivor_absent ? 1 : 0,
            reap_seen ? 1 : 0,
            reap_normal ? 1 : 0,
            sintra::s_mproc == nullptr ? 1 : 0);
    }
    return valid;
}

bool run_publication_occurrence_identity_race(int argc, char* argv[])
{
    sintra::init(argc, argv);
    constexpr uint64_t custody_identity = 0x4a71u;
    constexpr uint32_t predecessor_occurrence = 7;
    constexpr uint32_t replacement_occurrence = 8;
    const auto process_iid = sintra::s_mproc_id;
    const auto process_index = sintra::get_process_index(process_iid);
    const auto predecessor_iid = sintra::compose_instance(process_index, 0x6a41u);
    const auto replacement_iid = sintra::compose_instance(process_index, 0x6a42u);
    const std::string predecessor_name =
        "managed_child_publication_predecessor_" + std::to_string(process_iid);
    const std::string replacement_name =
        "managed_child_publication_replacement_" + std::to_string(process_iid);

    Publication_identity_gate gate;
    gate.predecessor_name = predecessor_name;
    gate.replacement_name = replacement_name;
    s_publication_identity_gate = &gate;
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &hold_predecessor_publication_identity, std::memory_order_release);

    sintra::instance_id_type predecessor_result = sintra::invalid_instance_id;
    std::thread predecessor([&]() {
        predecessor_result =
            sintra::s_coord->publish_managed_child_transceiver_for_test(
                sintra::make_user_type_id(1002),
                predecessor_iid,
                predecessor_name,
                custody_identity,
                process_iid,
                predecessor_occurrence);
    });
    bool predecessor_captured = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        predecessor_captured = gate.changed.wait_for(lock, 5s, [&]() {
            return gate.predecessor_captured;
        });
    }

    const auto replacement_result =
        sintra::s_coord->publish_managed_child_transceiver_for_test(
            sintra::make_user_type_id(1002),
            replacement_iid,
            replacement_name,
            custody_identity,
            process_iid,
            replacement_occurrence);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release_predecessor = true;
        gate.changed.notify_all();
    }
    predecessor.join();

    std::atomic<bool> cancelled{false};
    const auto predecessor_exact = sintra::detail::Managed_child_readiness_access::resolve(
        sintra::s_coord,
        predecessor_name,
        custody_identity,
        process_iid,
        predecessor_occurrence,
        cancelled);
    const auto predecessor_as_replacement =
        sintra::detail::Managed_child_readiness_access::resolve(
            sintra::s_coord,
            predecessor_name,
            custody_identity,
            process_iid,
            replacement_occurrence,
            cancelled);
    const auto replacement_exact = sintra::detail::Managed_child_readiness_access::resolve(
        sintra::s_coord,
        replacement_name,
        custody_identity,
        process_iid,
        replacement_occurrence,
        cancelled);
    const auto replacement_as_predecessor =
        sintra::detail::Managed_child_readiness_access::resolve(
            sintra::s_coord,
            replacement_name,
            custody_identity,
            process_iid,
            predecessor_occurrence,
            cancelled);

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr, std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_name_retired.store(
        &observe_occurrence_publication_retirement, std::memory_order_release);
    const bool predecessor_unpublished =
        sintra::Coordinator::rpc_unpublish_transceiver(
            sintra::s_coord_id, predecessor_iid);
    const bool replacement_unpublished =
        sintra::Coordinator::rpc_unpublish_transceiver(
            sintra::s_coord_id, replacement_iid);
    const bool predecessor_resolution_retired =
        sintra::detail::Managed_child_readiness_access::resolve(
            sintra::s_coord,
            predecessor_name,
            custody_identity,
            process_iid,
            predecessor_occurrence,
            cancelled) == sintra::invalid_instance_id;
    const bool replacement_resolution_retired =
        sintra::detail::Managed_child_readiness_access::resolve(
            sintra::s_coord,
            replacement_name,
            custody_identity,
            process_iid,
            replacement_occurrence,
            cancelled) == sintra::invalid_instance_id;
    const bool finalized = settle_detail_finalize("publication_occurrence_identity");
    bool predecessor_retired = false;
    bool replacement_retired = false;
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        predecessor_retired = gate.predecessor_retired;
        replacement_retired = gate.replacement_retired;
    }
    sintra::detail::test_hooks::s_coordinator_name_retired.store(
        nullptr, std::memory_order_release);
    s_publication_identity_gate = nullptr;

    const bool valid = predecessor_captured &&
        predecessor_result == predecessor_iid &&
        replacement_result == replacement_iid &&
        predecessor_exact == predecessor_iid &&
        predecessor_as_replacement == sintra::invalid_instance_id &&
        replacement_exact == replacement_iid &&
        replacement_as_predecessor == sintra::invalid_instance_id &&
        predecessor_unpublished && replacement_unpublished &&
        predecessor_resolution_retired && replacement_resolution_retired &&
        predecessor_retired && replacement_retired &&
        finalized;
    if (!valid) {
        std::fprintf(stderr,
            "PUBLICATION_OCCURRENCE_INVALID captured=%d predecessor_commit=%d "
            "replacement_commit=%d predecessor_exact=%d predecessor_cross=%d "
            "replacement_exact=%d replacement_cross=%d predecessor_unpublished=%d "
            "replacement_unpublished=%d predecessor_resolution_retired=%d "
            "replacement_resolution_retired=%d predecessor_retired=%d "
            "replacement_retired=%d finalized=%d\n",
            predecessor_captured ? 1 : 0,
            predecessor_result == predecessor_iid ? 1 : 0,
            replacement_result == replacement_iid ? 1 : 0,
            predecessor_exact == predecessor_iid ? 1 : 0,
            predecessor_as_replacement == sintra::invalid_instance_id ? 1 : 0,
            replacement_exact == replacement_iid ? 1 : 0,
            replacement_as_predecessor == sintra::invalid_instance_id ? 1 : 0,
            predecessor_unpublished ? 1 : 0,
            replacement_unpublished ? 1 : 0,
            predecessor_resolution_retired ? 1 : 0,
            replacement_resolution_retired ? 1 : 0,
            predecessor_retired ? 1 : 0,
            replacement_retired ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid;
}

bool run_immediate_reaped_classification(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    (void)binary_path;
    return true;
#else
    sintra::init(argc, argv);
    size_t roster_size_before = 0;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_spawned_child_pids_mutex);
        roster_size_before = sintra::s_mproc->m_spawned_child_pids.size();
    }
    s_immediate_exit_pid.store(-1, std::memory_order_relaxed);
    s_immediate_exit_observed.store(false, std::memory_order_relaxed);
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &observe_posix_reap, std::memory_order_release);
    sintra::testing::set_spawn_detached_exec_handshake(
        &wait_after_immediate_exec_handshake);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_immediate_exit_child_flag};
    options.process_instance_id = sintra::make_process_instance_id();
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    const auto observed = custody.status();
    const pid_t child_pid = s_immediate_exit_pid.load(std::memory_order_acquire);

    bool roster_unchanged = false;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_spawned_child_pids_mutex);
        roster_unchanged =
            sintra::s_mproc->m_spawned_child_pids.size() == roster_size_before &&
            std::none_of(
                sintra::s_mproc->m_spawned_child_pids.begin(),
                sintra::s_mproc->m_spawned_child_pids.end(),
                [&](const sintra::Managed_process::Spawned_child_reap_slot& slot) {
                    return slot.pid == child_pid;
                });
    }
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    const bool reap_normal = posix_reap_normal();
    const bool survivor_absent = child_pid > 0 &&
        !sintra::is_process_alive(static_cast<uint32_t>(child_pid));
    sintra::testing::set_spawn_detached_exec_handshake(nullptr);
    clear_posix_reap();
    const bool finalized = settle_detail_finalize("immediate_reaped_classification");

    return
        s_immediate_exit_observed.load(std::memory_order_acquire) &&
        custody                                                   &&
        observed.created_occurrences == 1                         &&
        observed.exited_occurrences  == 1                         &&
        observed.release_state == sintra::Managed_child_release_state::open &&
        released.release_state == sintra::Managed_child_release_state::complete &&
        released.created_occurrences == 1                         &&
        released.exited_occurrences  == 1                         &&
        roster_unchanged                                          &&
        reap_normal                                               &&
        survivor_absent                                           &&
        finalized;
#endif
}

bool run_concurrent_posix_roster_reservations(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    (void)binary_path;
    return true;
#else
    sintra::init(argc, argv);
    const std::array markers{
        unique_marker("posix_roster_first"),
        unique_marker("posix_roster_second")};
    const std::array releases{
        release_marker(markers[0]),
        release_marker(markers[1])};
    std::error_code ec;
    for (size_t i = 0; i < 2; ++i) {
        std::filesystem::remove(markers[i], ec);
        std::filesystem::remove(releases[i], ec);
    }

    const std::array piids{
        sintra::make_process_instance_id(),
        sintra::make_process_instance_id()};
    Roster_reservation_gate gate;
    gate.expected_iids = piids;
    s_roster_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_roster_reserved.store(
        &hold_roster_reservation, std::memory_order_release);

    std::array<sintra::Managed_child_custody, 2> custodies;
    std::array<std::thread, 2> callers;
    for (size_t i = 0; i < 2; ++i) {
        callers[i] = std::thread([&, i]() {
            sintra::Spawn_options options;
            options.binary_path = binary_path;
            options.args = {k_native_bound_child_flag, markers[i].string()};
            options.process_instance_id = piids[i];
            options.readiness_instance_name =
                "managed_child_posix_roster_never_" + std::to_string(i);
            options.lifetime.enable_lifeline = false;
            custodies[i] = sintra::spawn_swarm_process(options);
        });
    }

    bool two_reserved = false;
    std::array<uint64_t, 2> reservation_ids{};
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        two_reserved = gate.changed.wait_for(lock, 5s, [&]() {
            return gate.reservations.size() == 2;
        });
        if (two_reserved) {
            reservation_ids = {gate.reservations[0], gate.reservations[1]};
        }
    }

    bool exact_placeholders = two_reserved &&
        reservation_ids[0] != 0 && reservation_ids[1] != 0 &&
        reservation_ids[0] != reservation_ids[1];
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_spawned_child_pids_mutex);
        for (auto reservation : reservation_ids) {
            const auto slot = std::find_if(
                sintra::s_mproc->m_spawned_child_pids.begin(),
                sintra::s_mproc->m_spawned_child_pids.end(),
                [&](const sintra::Managed_process::Spawned_child_reap_slot& candidate) {
                    return candidate.reservation_id == reservation && candidate.pid == 0;
                });
            exact_placeholders = exact_placeholders &&
                slot != sintra::s_mproc->m_spawned_child_pids.end();
        }
    }
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
        gate.changed.notify_all();
    }
    for (auto& caller : callers) {
        caller.join();
    }
    sintra::detail::test_hooks::s_managed_child_roster_reserved.store(
        nullptr, std::memory_order_release);
    s_roster_gate = nullptr;

    std::array<std::optional<process_identity_t>, 2> identities;
    for (size_t i = 0; i < 2; ++i) {
        if (wait_for_file(markers[i], 5s)) {
            identities[i] = read_process_identity(markers[i]);
        }
    }
    if (identities[0] && identities[1]) {
        arm_concurrent_posix_reaps(*identities[0], *identities[1]);
    }
    const bool releases_written =
        write_release_marker(markers[0]) && write_release_marker(markers[1]);

    std::array<sintra::Managed_child_status, 2> released;
    const auto release_deadline = std::chrono::steady_clock::now() + 5s;
    std::array<std::thread, 2> release_callers;
    for (size_t i = 0; i < 2; ++i) {
        release_callers[i] = std::thread([&, i]() {
            do {
                const auto attempt_deadline = std::min(
                    release_deadline,
                    std::chrono::steady_clock::now() + 250ms);
                released[i] = custodies[i].release_until(attempt_deadline);
            }
            while (released[i].release_state !=
                       sintra::Managed_child_release_state::complete &&
                std::chrono::steady_clock::now() < release_deadline);
        });
    }
    for (auto& caller : release_callers) {
        caller.join();
    }
    const bool releases_terminal =
        released[0].release_state == sintra::Managed_child_release_state::complete &&
        released[1].release_state == sintra::Managed_child_release_state::complete;

    bool survivors_absent = releases_terminal;
    for (size_t i = 0; i < 2; ++i) {
        survivors_absent = survivors_absent && identities[i] &&
            exact_process_is_absent(*identities[i]);
    }
    const bool reaps_normal = identities[0] && identities[1] &&
        concurrent_posix_reaps_normal();
    clear_concurrent_posix_reaps();

    bool reservations_retired = true;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_spawned_child_pids_mutex);
        for (auto reservation : reservation_ids) {
            reservations_retired = reservations_retired &&
                std::none_of(
                    sintra::s_mproc->m_spawned_child_pids.begin(),
                    sintra::s_mproc->m_spawned_child_pids.end(),
                    [&](const sintra::Managed_process::Spawned_child_reap_slot& slot) {
                        return slot.reservation_id == reservation;
                    });
        }
    }
    const bool finalized = settle_detail_finalize("concurrent_posix_roster");
    for (size_t i = 0; i < 2; ++i) {
        std::filesystem::remove(markers[i], ec);
        std::filesystem::remove(releases[i], ec);
    }

    return exact_placeholders && releases_written &&
        released[0].release_state == sintra::Managed_child_release_state::complete &&
        released[1].release_state == sintra::Managed_child_release_state::complete &&
        released[0].created_occurrences == 1 &&
        released[1].created_occurrences == 1 &&
        released[0].exited_occurrences == 1 &&
        released[1].exited_occurrences == 1 &&
        survivors_absent && reaps_normal && reservations_retired && finalized;
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == k_recovery_recipe_exposure_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            sintra::init(argc, argv);
            const auto occurrence = sintra::s_recovery_occurrence;
            if (occurrence == 0) {
                sintra::enable_recovery();
            }
            const auto exact_marker = occurrence_marker(marker, occurrence);
            const bool identity_written = write_process_identity(exact_marker);
            if (occurrence == 0) {
                const bool crash_requested = wait_for_file(
                    exit_marker(exact_marker), 15s);
                if (!identity_written || !crash_requested) {
                    return 3;
                }
                sintra::disable_debug_pause_for_current_process();
                sintra::test::prepare_for_intentional_crash(
                    "recovery recipe exposure");
                std::abort();
            }
            const bool released = occurrence == 1 && wait_for_file(
                release_marker(exact_marker), 15s);
            const bool finalized = settle_detail_finalize(
                "recovery_recipe_exposure_child");
            return identity_written && released && finalized ? 0 : 3;
        }
        if (std::string(argv[i]) == k_post_native_recovery_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            sintra::init(argc, argv);
            const auto occurrence = sintra::s_recovery_occurrence;
            if (occurrence == 0) {
                sintra::enable_recovery();
            }
            const auto exact_marker = occurrence_marker(marker, occurrence);
            const bool identity_written = write_process_identity(exact_marker);
            if (occurrence < 2) {
                const bool crash_requested =
                    wait_for_file(exit_marker(exact_marker), 15s);
                if (!identity_written || !crash_requested) {
                    return 3;
                }
                sintra::disable_debug_pause_for_current_process();
                sintra::test::prepare_for_intentional_crash(
                    "post-native recovery advance");
                std::abort();
            }
            const bool released = wait_for_file(
                release_marker(exact_marker), 15s);
            const bool finalized = settle_detail_finalize(
                "post_native_recovery_child");
            return
                identity_written &&
                occurrence == 2  &&
                released         &&
                finalized
                    ? 0
                    : 3;
        }
        if (std::string(argv[i]) == k_child_flag) {
            std::ofstream out(argv[i + 1], std::ios::binary | std::ios::trunc);
            out << "unexpected_child=1\n";
            return 2;
        }
        if (std::string(argv[i]) == k_native_bound_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            const bool identity_written = write_process_identity(marker);
            const bool released = wait_for_file(release_marker(marker), 10s);
            return identity_written && released ? 0 : 3;
        }
        if (std::string(argv[i]) == k_owned_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            const bool hold_after_finalize =
                i + 2 < argc &&
                std::string(argv[i + 2]) == k_hold_after_finalize_flag;
            sintra::init(argc, argv);
            const bool identity_written = write_process_identity(marker);
            const bool released = wait_for_file(release_marker(marker), 10s);
            const bool finalized = settle_detail_finalize("owned_child");
            const bool finalized_written =
                !hold_after_finalize ||
                (finalized && write_signal_marker(
                    finalized_marker(marker), "finalized"));
            const bool exit_released =
                !hold_after_finalize || wait_for_file(exit_marker(marker), 15s);
            return identity_written && released && finalized &&
                finalized_written && exit_released ? 0 : 3;
        }
        if (std::string(argv[i]) == k_prepublication_exit_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            const bool identity_written = write_process_identity(marker);
            std::this_thread::sleep_for(250ms);
            return identity_written ? 0 : 3;
        }
        if (std::string(argv[i]) == k_immediate_exit_child_flag) {
            return 0;
        }
        if (std::string(argv[i]) == k_readiness_identity_child_flag &&
            i + 2 < argc)
        {
            const std::filesystem::path marker = argv[i + 1];
            const std::string target_name = argv[i + 2];
            sintra::init(argc, argv);
            bool assigned = false;
            bool identity_written = false;
            bool released = false;
            {
                Unrelated_publication_target target;
                assigned = target.assign_name(target_name);
                identity_written = write_process_identity(marker);
                released = wait_for_file(release_marker(marker), 10s);
            }
            const bool finalized = settle_detail_finalize("readiness_identity_child");
            return assigned && identity_written && released && finalized ? 0 : 3;
        }
        if (std::string(argv[i]) == k_readiness_cancellation_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            const bool identity_written = write_process_identity(marker);
            std::this_thread::sleep_for(1s);
            return identity_written ? 0 : 3;
        }
    }

    const std::string binary_path = std::filesystem::absolute(argv[0]).string();
    const bool recovery_race = run_recovery_create_release_race(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool recovery_recipe_exposure = run_recovery_recipe_exposure_race(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool deadline_race = run_deadline_setup_shutdown_retry(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool pre_create_exception = run_pre_create_exception(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool post_native_exception = run_owned_native_exception(
        argc, argv, binary_path,
        "post_native_exception",
        sintra::detail::test_hooks::k_managed_child_fail_post_native_setup);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool post_native_recovery_advance =
        run_post_native_recovery_occurrence_advance(argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool observer_start_failure = run_owned_native_exception(
        argc, argv, binary_path,
        "observer_start_failure",
        sintra::detail::test_hooks::k_managed_child_fail_native_observer_start);
    if (!s_teardown_settled) {
        return 2;
    }
    const auto release_worker_retry = run_release_worker_retry(
        argc,
        argv,
        binary_path,
        "release_worker_retry",
        sintra::detail::test_hooks::k_managed_child_fail_release_worker,
        sintra::Managed_child_failure_kind::release_worker_execution);
    if (!s_teardown_settled) {
        return 2;
    }
    const auto release_worker_start_retry = run_release_worker_retry(
        argc,
        argv,
        binary_path,
        "release_worker_start_retry",
        sintra::detail::test_hooks::k_managed_child_fail_release_worker_start,
        sintra::Managed_child_failure_kind::release_worker_start);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool prepublication_exit = run_prepublication_exit_convergence(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool split_transport_retirement = run_split_transport_retirement(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool prepublication_publish_race = run_prepublication_publish_race(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool unrelated_readiness_rejection = run_unrelated_readiness_rejection(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool exact_readiness_acceptance = run_exact_readiness_acceptance(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool unbounded_readiness_cancellation = run_unbounded_readiness_cancellation(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool publication_occurrence_identity =
        run_publication_occurrence_identity_race(argc, argv);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool immediate_reaped_classification =
        run_immediate_reaped_classification(argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool concurrent_posix_roster = run_concurrent_posix_roster_reservations(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }

    const bool unrelated_setup_race_green =
        recovery_race && recovery_recipe_exposure && deadline_race &&
        pre_create_exception &&
        post_native_exception && post_native_recovery_advance &&
        observer_start_failure &&
        prepublication_exit && split_transport_retirement &&
        prepublication_publish_race && unrelated_readiness_rejection &&
        exact_readiness_acceptance && unbounded_readiness_cancellation &&
        publication_occurrence_identity &&
        immediate_reaped_classification &&
        concurrent_posix_roster;

    if (unrelated_setup_race_green &&
        release_worker_retry ==
            Release_worker_retry_result::red_missing_report &&
        release_worker_start_retry ==
            Release_worker_retry_result::red_missing_report)
    {
        std::fprintf(
            stderr,
            "F04_RELEASE_FAILURE_RED_VALID body=red_missing_report "
            "start=red_missing_report unrelated_setup_race=green\n");
        return 4;
    }

    if (unrelated_setup_race_green &&
        release_worker_retry == Release_worker_retry_result::green &&
        release_worker_start_retry == Release_worker_retry_result::green)
    {
        std::printf(
            "SETUP_RACE_GREEN_VALID recovery_pending=1 release_waited=1 "
            "recovery_no_child=1 recovery_recipe_preexposed=1 "
            "deadline_bounded=1 shutdown_retry_no_throw=1 "
            "shutdown_retry_resumed_finalize=1 finalize_retained=1 final_shutdown=1 "
            "pre_create_exception_no_child=1 post_native_exception_owned=1 "
            "post_native_recovery_advanced=1 "
            "observer_start_failure_owned=1 release_worker_retry=1 "
            "release_worker_start_retry=1 "
            "prepublication_exit_converged=1 split_transport_retirement=1 "
            "split_transport_join_false=1 total_stop_deadline_bounded=1 "
            "distinct_release_retry=1 communication_worker_retry=1 "
            "inflight_publish_canonically_retired=1 "
            "readiness_unrelated_rejected=1 readiness_exact_occurrence=1 "
            "readiness_release_cancelled=1 "
            "publication_occurrence_atomic=1 "
            "immediate_created_reaped=%s immediate_reap_count=%s "
            "posix_concurrent_roster=%s reap_count_per_owned_phase=%s survivors=0\n",
#ifdef _WIN32
            "not_applicable",
            "not_applicable",
            "not_applicable",
            "not_applicable"
#else
            "1",
            "1",
            "2",
            "1"
#endif
            );
        return 0;
    }

    std::fprintf(stderr,
        "SETUP_RACE_INVALID recovery_race=%d recovery_recipe_exposure=%d "
        "deadline_race=%d "
        "pre_create_exception=%d post_native_exception=%d "
        "post_native_recovery_advance=%d "
        "observer_start_failure=%d release_worker_retry=%s "
        "release_worker_start_retry=%s "
        "prepublication_exit=%d split_transport_retirement=%d "
        "prepublication_publish_race=%d readiness_unrelated=%d "
        "readiness_exact=%d readiness_cancelled=%d immediate_reaped=%d "
        "publication_occurrence=%d concurrent_posix_roster=%d\n",
        recovery_race ? 1 : 0,
        recovery_recipe_exposure ? 1 : 0,
        deadline_race ? 1 : 0,
        pre_create_exception ? 1 : 0, post_native_exception ? 1 : 0,
        post_native_recovery_advance ? 1 : 0,
        observer_start_failure ? 1 : 0,
        release_worker_retry_result_name(release_worker_retry),
        release_worker_retry_result_name(release_worker_start_retry),
        prepublication_exit ? 1 : 0,
        split_transport_retirement ? 1 : 0,
        prepublication_publish_race ? 1 : 0,
        unrelated_readiness_rejection ? 1 : 0,
        exact_readiness_acceptance ? 1 : 0,
        unbounded_readiness_cancellation ? 1 : 0,
        immediate_reaped_classification ? 1 : 0,
        publication_occurrence_identity ? 1 : 0,
        concurrent_posix_roster ? 1 : 0);
    return 2;
}
