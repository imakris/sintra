// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

constexpr const char* k_child_flag = "--managed-child-setup-race-child";
constexpr const char* k_native_bound_child_flag =
    "--managed-child-native-bound-failure-child";
constexpr const char* k_owned_child_flag = "--managed-child-owned-failure-child";

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
    sintra::instance_id_type    expected_iid = sintra::invalid_instance_id;
    uint32_t                    expected_occurrence = 0;
    unsigned                    remaining = 0;
    unsigned                    hits = 0;
};

Failure_plan* s_failure_plan = nullptr;

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
    auto* plan = s_failure_plan;
    if (!plan || !stage) {
        return false;
    }
    std::lock_guard<std::mutex> lock(plan->mutex);
    if (plan->remaining == 0 || process_iid != plan->expected_iid ||
        occurrence != plan->expected_occurrence ||
        std::string_view(stage) != plan->stage)
    {
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

struct Child_identity
{
    int       pid = -1;
    uint64_t  start_stamp = 0;
};

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
        return record->release_complete && record->readiness_observer_complete;
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

bool wait_for_file(const std::filesystem::path& path, std::chrono::milliseconds timeout)
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

bool write_child_identity(const std::filesystem::path& marker)
{
    const auto stamp = sintra::current_process_start_stamp();
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    if (!out || !stamp) {
        return false;
    }
    out << "pid=" << sintra::detail::get_current_process_id() << '\n'
        << "start_stamp=" << *stamp << '\n'
        << "complete=1\n";
    return static_cast<bool>(out);
}

std::optional<Child_identity> read_child_identity(const std::filesystem::path& marker)
{
    std::ifstream in(marker, std::ios::binary);
    Child_identity identity;
    bool complete = false;
    std::string line;
    try {
        while (std::getline(in, line)) {
            const auto split = line.find('=');
            if (split == std::string::npos) {
                return std::nullopt;
            }
            const auto key = line.substr(0, split);
            const auto value = line.substr(split + 1);
            if (key == "pid") {
                identity.pid = std::stoi(value);
            }
            else if (key == "start_stamp") {
                identity.start_stamp = std::stoull(value);
            }
            else if (key == "complete") {
                complete = value == "1";
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }
    if (!complete || identity.pid <= 0 || identity.start_stamp == 0) {
        return std::nullopt;
    }
    return identity;
}

bool exact_child_absent(const Child_identity& identity)
{
    if (!sintra::is_process_alive(static_cast<uint32_t>(identity.pid))) {
        return true;
    }
    const auto observed = sintra::query_process_start_stamp(
        static_cast<uint32_t>(identity.pid));
    return !observed || *observed != identity.start_stamp;
}

bool wait_for_child_absent(
    const Child_identity& identity,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (exact_child_absent(identity)) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return exact_child_absent(identity);
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

void reset_failure_hook()
{
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    s_failure_plan = nullptr;
}

#ifndef _WIN32
void arm_posix_reap(const Child_identity& identity)
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

void clear_posix_reap()
{
    sintra::detail::test_hooks::s_child_reaped.store(
        nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
}

void arm_concurrent_posix_reaps(
    const Child_identity& first,
    const Child_identity& second)
{
    const std::array<Child_identity, 2> identities{first, second};
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
        result = sintra::s_mproc->spawn_swarm_process(args);
    });

    const bool setup_held = wait_for_gate(gate);
    bool pending_before_release = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        pending_before_release = custody->occurrences.size() == 1 &&
            custody->occurrences.front().setup ==
                sintra::detail::Managed_child_occurrence_record::setup_state::pending;
    }

    sintra::s_mproc->request_child_custody_release(custody, true);
    std::this_thread::sleep_for(100ms);
    bool incomplete_while_held = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        incomplete_while_held = custody->release_requested &&
            !custody->release_complete &&
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
            !custody->occurrences.front().os_process_created;
    }

    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        nullptr, std::memory_order_release);
    s_gate = nullptr;
    const bool finalized = settle_detail_finalize("recovery_create_release");
    const bool no_child_after = marker_absent(marker);
    std::filesystem::remove(marker, ec);

    return setup_held && pending_before_release && incomplete_while_held &&
        no_child_while_held && !result.success && !result.os_process_created &&
        resolved_no_child && release_completed && finalized && no_child_after;
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
    options.wait_for_instance_name = "managed_child_setup_race_never_published";
    options.wait_timeout = 200ms;
    options.lifetime.enable_lifeline = false;

    const auto started = std::chrono::steady_clock::now();
    auto custody = sintra::spawn_swarm_process(options);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto observation = sintra::observe_managed_child(custody);
    const bool setup_held = wait_for_gate(gate);
    const bool caller_bounded =
        elapsed >= 150ms && elapsed <= 1000ms && observation.accepted &&
        observation.admitted_occurrences == 1 &&
        observation.created_occurrences == 0 &&
        observation.release_requested && !observation.release_complete;

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
    const auto released = sintra::wait_managed_child(
        custody, std::chrono::steady_clock::now() + 5s);
    sintra::detail::test_hooks::s_managed_child_reader_setup.store(
        nullptr, std::memory_order_release);
    s_gate = nullptr;

    const bool final_shutdown = settle_runtime_teardown(
        "deadline_setup_shutdown",
        []() { return sintra::shutdown(); });
    const bool no_child_after = marker_absent(marker);
    std::filesystem::remove(marker, ec);

    return setup_held && caller_bounded && retained_while_held &&
        released.release_complete && final_shutdown && !shutdown_threw &&
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
    s_failure_plan = &plan;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.wait_for_instance_name = "managed_child_pre_create_never_published";
    options.wait_timeout = 300ms;
    options.lifetime.enable_lifeline = false;

    bool threw = false;
    sintra::Managed_child_custody custody;
    const auto started = std::chrono::steady_clock::now();
    try {
        custody = sintra::spawn_swarm_process(options);
    }
    catch (...) {
        threw = true;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto released = sintra::wait_managed_child(
        custody, std::chrono::steady_clock::now() + 5s);
    const auto hits = failure_hits(plan);
    reset_failure_hook();
    const bool finalized = settle_detail_finalize("pre_create_exception");
    const bool marker_missing = marker_absent(marker);
    std::filesystem::remove(marker, ec);

    return !threw && elapsed < 1s && released.accepted &&
        released.admitted_occurrences == 1 &&
        released.created_occurrences == 0 && released.release_requested &&
        released.release_complete && hits == 1 && marker_missing && finalized;
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
    s_failure_plan = &plan;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_native_bound_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.wait_for_instance_name =
        std::string("managed_child_") + phase + "_never_published";
    options.wait_timeout = 500ms;
    options.lifetime.enable_lifeline = false;

    bool threw = false;
    sintra::Managed_child_custody custody;
    try {
        custody = sintra::spawn_swarm_process(options);
    }
    catch (...) {
        threw = true;
    }
    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_child_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif
    const bool release_written = write_release_marker(marker);
    const auto released = sintra::wait_managed_child(
        custody, std::chrono::steady_clock::now() + 5s);
    const bool survivor_absent = identity && wait_for_child_absent(*identity, 5s);
    const auto hits = failure_hits(plan);
    reset_failure_hook();
#ifndef _WIN32
    const bool reap_normal = identity && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_normal = true;
#endif
    const bool finalized = settle_detail_finalize(phase);
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    return !threw && hits == 1 && identity && release_written &&
        released.accepted && released.admitted_occurrences == 1 &&
        released.created_occurrences == 1 && released.exited_occurrences == 1 &&
        released.release_requested && released.release_complete &&
        survivor_absent && reap_normal && finalized;
}

bool run_release_worker_retry(
    int argc,
    char* argv[],
    const std::string& binary_path)
{
    sintra::init(argc, argv);
    const auto marker = unique_marker("release_retry");
    const auto release = release_marker(marker);
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const auto piid = sintra::make_process_instance_id();
    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_owned_child_flag, marker.string()};
    options.process_instance_id = piid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_child_identity(marker)
        : std::nullopt;
#ifndef _WIN32
    if (identity) {
        arm_posix_reap(*identity);
    }
#endif

    Failure_plan plan;
    plan.stage = sintra::detail::test_hooks::k_managed_child_fail_release_worker;
    plan.expected_iid = piid;
    plan.expected_occurrence = 0;
    plan.remaining = 1;
    s_failure_plan = &plan;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_managed_child_failure, std::memory_order_release);

    const auto first = sintra::release_managed_child(
        custody, std::chrono::steady_clock::now() + 250ms);
    const auto hits_after_first = failure_hits(plan);
    const bool release_written = write_release_marker(marker);
    const auto second = sintra::retry_managed_child_release(
        custody, std::chrono::steady_clock::now() + 5s);
    const bool survivor_absent = identity && wait_for_child_absent(*identity, 5s);
    reset_failure_hook();
#ifndef _WIN32
    const bool reap_normal = identity && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_normal = true;
#endif
    const bool finalized = settle_detail_finalize("release_worker_retry");
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    return identity && hits_after_first == 1 && first.accepted &&
        first.release_requested && !first.release_complete && release_written &&
        second.release_complete && second.created_occurrences == 1 &&
        second.exited_occurrences == 1 && survivor_absent && reap_normal && finalized;
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
    s_failure_plan = &plan;

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
    options.wait_for_instance_name = "managed_child_prepublication_never_published";
    options.wait_timeout = 500ms;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    bool first_miss = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        first_miss = gate.changed.wait_for(lock, 5s, [&]() {
            return gate.first_miss;
        });
    }

    sintra::instance_id_type publish_result = sintra::invalid_instance_id;
    std::thread publisher([&]() {
        publish_result = sintra::s_coord->publish_transceiver_for_test(
            sintra::make_user_type_id(1001), piid, published_name);
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
    const auto held_observation = sintra::observe_managed_child(custody);

    const bool identity_written = wait_for_file(marker, 5s);
    const auto identity = identity_written
        ? read_child_identity(marker)
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

    const auto released = sintra::wait_managed_child(
        custody, std::chrono::steady_clock::now() + 5s);
    const bool survivor_absent = identity && wait_for_child_absent(*identity, 5s);
    const bool canonical_absence =
        assigned_name_absent(published_name) && process_registry_absent(piid);

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_prepublication_cleanup.store(
        nullptr, std::memory_order_release);
    s_prepublication_gate = nullptr;
    reset_failure_hook();
#ifndef _WIN32
    const bool reap_normal = identity && posix_reap_normal();
    clear_posix_reap();
#else
    const bool reap_normal = true;
#endif
    const bool finalized = settle_detail_finalize("prepublication_publish_race");
    std::filesystem::remove(marker, ec);
    std::filesystem::remove(release, ec);

    const bool valid = first_miss && reader_terminal && publish_held &&
        !held_observation.release_complete && publish_result == piid &&
        release_written && released.release_complete && canonical_absence &&
        survivor_absent && reap_normal && finalized;
    if (!valid) {
        std::fprintf(stderr,
            "PREPUBLICATION_INVALID first_miss=%d reader_terminal=%d publish_held=%d "
            "held_incomplete=%d publish_result=%d release_written=%d release_complete=%d "
            "canonical_absence=%d survivor_absent=%d reap_normal=%d finalized=%d\n",
            first_miss ? 1 : 0, reader_terminal ? 1 : 0, publish_held ? 1 : 0,
            !held_observation.release_complete ? 1 : 0,
            publish_result == piid ? 1 : 0, release_written ? 1 : 0,
            released.release_complete ? 1 : 0, canonical_absence ? 1 : 0,
            survivor_absent ? 1 : 0, reap_normal ? 1 : 0, finalized ? 1 : 0);
    }
    return valid;
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
            options.wait_for_instance_name =
                "managed_child_posix_roster_never_" + std::to_string(i);
            options.wait_timeout = 2s;
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

    std::array<std::optional<Child_identity>, 2> identities;
    for (size_t i = 0; i < 2; ++i) {
        if (wait_for_file(markers[i], 5s)) {
            identities[i] = read_child_identity(markers[i]);
        }
    }
    if (identities[0] && identities[1]) {
        arm_concurrent_posix_reaps(*identities[0], *identities[1]);
    }
    const bool releases_written =
        write_release_marker(markers[0]) && write_release_marker(markers[1]);

    std::array<sintra::Managed_child_custody_observation, 2> released;
    bool survivors_absent = true;
    for (size_t i = 0; i < 2; ++i) {
        released[i] = sintra::wait_managed_child(
            custodies[i], std::chrono::steady_clock::now() + 5s);
        survivors_absent = survivors_absent && identities[i] &&
            wait_for_child_absent(*identities[i], 5s);
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
        released[0].release_complete && released[1].release_complete &&
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
        if (std::string(argv[i]) == k_child_flag) {
            std::ofstream out(argv[i + 1], std::ios::binary | std::ios::trunc);
            out << "unexpected_child=1\n";
            return 2;
        }
        if (std::string(argv[i]) == k_native_bound_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            const bool identity_written = write_child_identity(marker);
            const bool released = wait_for_file(release_marker(marker), 10s);
            return identity_written && released ? 0 : 3;
        }
        if (std::string(argv[i]) == k_owned_child_flag) {
            const std::filesystem::path marker = argv[i + 1];
            sintra::init(argc, argv);
            const bool identity_written = write_child_identity(marker);
            const bool released = wait_for_file(release_marker(marker), 10s);
            const bool finalized = settle_detail_finalize("owned_child");
            return identity_written && released && finalized ? 0 : 3;
        }
    }

    const std::string binary_path = std::filesystem::absolute(argv[0]).string();
    const bool recovery_race = run_recovery_create_release_race(
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
    const bool observer_start_failure = run_owned_native_exception(
        argc, argv, binary_path,
        "observer_start_failure",
        sintra::detail::test_hooks::k_managed_child_fail_native_observer_start);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool release_worker_retry = run_release_worker_retry(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool prepublication_publish_race = run_prepublication_publish_race(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }
    const bool concurrent_posix_roster = run_concurrent_posix_roster_reservations(
        argc, argv, binary_path);
    if (!s_teardown_settled) {
        return 2;
    }

    if (recovery_race && deadline_race && pre_create_exception &&
        post_native_exception && observer_start_failure && release_worker_retry &&
        prepublication_publish_race && concurrent_posix_roster)
    {
        std::printf(
            "SETUP_RACE_GREEN_VALID recovery_pending=1 release_waited=1 "
            "recovery_no_child=1 deadline_bounded=1 shutdown_retry_no_throw=1 "
            "shutdown_retry_resumed_finalize=1 finalize_retained=1 final_shutdown=1 "
            "pre_create_exception_no_child=1 post_native_exception_owned=1 "
            "observer_start_failure_owned=1 release_worker_retry=1 "
            "inflight_publish_canonically_retired=1 "
            "posix_concurrent_roster=%s reap_count_per_owned_phase=%s survivors=0\n",
#ifdef _WIN32
            "not_applicable",
            "not_applicable"
#else
            "2",
            "1"
#endif
            );
        return 0;
    }

    std::fprintf(stderr,
        "SETUP_RACE_INVALID recovery_race=%d deadline_race=%d "
        "pre_create_exception=%d post_native_exception=%d "
        "observer_start_failure=%d release_worker_retry=%d "
        "prepublication_publish_race=%d concurrent_posix_roster=%d\n",
        recovery_race ? 1 : 0, deadline_race ? 1 : 0,
        pre_create_exception ? 1 : 0, post_native_exception ? 1 : 0,
        observer_start_failure ? 1 : 0, release_worker_retry ? 1 : 0,
        prepublication_publish_race ? 1 : 0,
        concurrent_posix_roster ? 1 : 0);
    return 2;
}
