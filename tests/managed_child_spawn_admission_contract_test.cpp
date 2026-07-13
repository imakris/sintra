#define SINTRA_ENABLE_TEST_HOOKS 1
#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

using sintra::test::managed_child::exact_process_is_live;

constexpr const char* k_worker_flag           = "--atom3-worker";
constexpr const char* k_unexpected_child_flag = "--atom3-unexpected-child";
constexpr const char* k_owned_child_flag      = "--atom3-owned-child";

struct child_identity_t
{
    int      pid = -1;
    uint64_t start_stamp = 0;
};

struct Failure_plan
{
    const char*                         stage = nullptr;
    sintra::instance_id_type            process_iid = sintra::invalid_instance_id;
    uint32_t                            occurrence = 0;
    std::atomic<unsigned>               remaining{0};
    const std::filesystem::path*        child_marker = nullptr;
};

Failure_plan* s_failure_plan = nullptr;

struct Post_spawn_gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

Post_spawn_gate* s_post_spawn_gate = nullptr;

void hold_public_sync_post_spawn(
    sintra::detail::Managed_child_post_spawn_stage stage,
    sintra::instance_id_type,
    uint32_t)
{
    auto* gate = s_post_spawn_gate;
    if (!gate || stage != sintra::detail::Managed_child_post_spawn_stage::
            public_sync_no_child)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->entered = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&]() { return gate->release; });
}

bool wait_for_file(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::error_code ec;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path, ec)) {
            return true;
        }
        ec.clear();
        std::this_thread::sleep_for(10ms);
    }
    return std::filesystem::exists(path, ec);
}

bool marker_absent(const std::filesystem::path& path)
{
    std::error_code ec;
    return !std::filesystem::exists(path, ec);
}

bool inject_setup_failure(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence) noexcept
{
    auto* plan = s_failure_plan;
    if (!plan || !plan->stage || std::string_view(stage) != plan->stage ||
        process_iid != plan->process_iid || occurrence != plan->occurrence)
    {
        return false;
    }

    unsigned remaining = plan->remaining.load(std::memory_order_acquire);
    while (
        remaining != 0 &&
        !plan->remaining.compare_exchange_weak(
            remaining, remaining - 1, std::memory_order_acq_rel))
    {}
    if (remaining == 0) {
        return false;
    }

    if (plan->child_marker) {
        wait_for_file(*plan->child_marker, 5s);
    }
    return true;
}

void reset_failure_hook()
{
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    s_failure_plan = nullptr;
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

bool settle_finalize()
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (sintra::detail::finalize()) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return sintra::detail::finalize();
}

bool write_child_identity(const std::filesystem::path& path)
{
    const auto pid = sintra::test::get_pid();
    const auto stamp = sintra::query_process_start_stamp(
        static_cast<uint32_t>(pid));
    if (!stamp) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << pid << ' ' << *stamp << '\n';
    out.flush();
    return out.good();
}

std::optional<child_identity_t> read_child_identity(
    const std::filesystem::path& path)
{
    child_identity_t identity;
    std::ifstream in(path, std::ios::binary);
    in >> identity.pid >> identity.start_stamp;
    if (!in || identity.pid <= 0 || identity.start_stamp == 0) {
        return std::nullopt;
    }
    return identity;
}

bool exact_child_absent(const child_identity_t& identity)
{
    return !exact_process_is_live(identity.pid, identity.start_stamp);
}

bool wait_for_exact_child_absence(
    const child_identity_t& identity,
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

bool run_malformed_id_rejections(
    const std::string& binary_path,
    const std::filesystem::path& marker)
{
    const std::array malformed_ids{
        sintra::compose_instance(0, 1),
        sintra::compose_instance(2, 2),
        sintra::any_local,
        sintra::any_remote};

    bool all_rejected = true;
    for (const auto process_iid : malformed_ids) {
        sintra::Spawn_options options;
        options.binary_path = binary_path;
        options.args = {k_unexpected_child_flag, marker.string()};
        options.process_instance_id = process_iid;
        options.lifetime.enable_lifeline = false;
        all_rejected = !sintra::spawn_swarm_process(options) && all_rejected;
    }
    std::this_thread::sleep_for(250ms);
    return all_rejected && marker_absent(marker);
}

bool run_pre_create_exception(
    const std::string& binary_path,
    const std::filesystem::path& marker)
{
    const auto process_iid = sintra::make_process_instance_id();
    Failure_plan plan;
    plan.stage = sintra::detail::test_hooks::k_managed_child_fail_pre_create_setup;
    plan.process_iid = process_iid;
    plan.remaining.store(1, std::memory_order_release);
    s_failure_plan = &plan;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_setup_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_unexpected_child_flag, marker.string()};
    options.process_instance_id = process_iid;
    options.lifetime.enable_lifeline = false;

    bool threw = false;
    sintra::Managed_child_custody custody;
    try {
        custody = sintra::spawn_swarm_process(options);
    }
    catch (...) {
        threw = true;
    }
    reset_failure_hook();
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    return
        !threw                                              &&
        custody                                             &&
        released.admitted_occurrences == 1                  &&
        released.created_occurrences  == 0                  &&
        released.release_state ==
            sintra::Managed_child_release_state::complete   &&
        observed_setup_exception(released, plan.stage)      &&
        plan.remaining.load(std::memory_order_acquire) == 0 &&
        marker_absent(marker);
}

bool run_post_native_exception(
    const std::string& binary_path,
    const std::filesystem::path& marker)
{
    const auto process_iid = sintra::make_process_instance_id();
    Failure_plan plan;
    plan.stage = sintra::detail::test_hooks::k_managed_child_fail_post_native_setup;
    plan.process_iid = process_iid;
    plan.remaining.store(1, std::memory_order_release);
    plan.child_marker = &marker;
    s_failure_plan = &plan;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_setup_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_owned_child_flag, marker.string()};
    options.process_instance_id = process_iid;
    options.lifetime.enable_lifeline = false;

    bool threw = false;
    sintra::Managed_child_custody custody;
    try {
        custody = sintra::spawn_swarm_process(options);
    }
    catch (...) {
        threw = true;
    }
    reset_failure_hook();
    const auto identity = read_child_identity(marker);
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 15s);
    const bool survivor_absent = identity && wait_for_exact_child_absence(*identity, 2s);
    return
        !threw                                              &&
        identity                                            &&
        custody                                             &&
        released.admitted_occurrences == 1                  &&
        released.created_occurrences  == 1                  &&
        released.exited_occurrences   == 1                  &&
        released.release_state ==
            sintra::Managed_child_release_state::complete   &&
        observed_setup_exception(released, plan.stage)      &&
        plan.remaining.load(std::memory_order_acquire) == 0 &&
        survivor_absent;
}

bool run_native_spawn_failure(const std::filesystem::path& missing_binary)
{
    sintra::Spawn_options options;
    options.binary_path = missing_binary.string();
    options.lifetime.enable_lifeline = false;

    const auto custody = sintra::spawn_swarm_process(options);
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    return custody && released.admitted_occurrences == 1 &&
        released.created_occurrences == 0 && released.release_state ==
            sintra::Managed_child_release_state::complete &&
        released.last_failure.kind ==
            sintra::Managed_child_failure_kind::native_spawn &&
        released.last_failure.occurrence == 0 &&
        released.last_failure.native_error != 0 &&
        !released.last_failure.message.empty();
}

bool run_sync_failure_finalize_fence(
    const std::filesystem::path& missing_binary)
{
    Post_spawn_gate gate;
    s_post_spawn_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_post_spawn.store(
        &hold_public_sync_post_spawn, std::memory_order_release);

    std::atomic<bool> spawn_returned{false};
    std::thread spawn_thread([&]() {
        sintra::Spawn_options options;
        options.binary_path = missing_binary.string();
        options.lifetime.enable_lifeline = false;
        (void)sintra::spawn_swarm_process(options);
        spawn_returned.store(true, std::memory_order_release);
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        entered = gate.changed.wait_for(
            lock, 5s, [&]() { return gate.entered; });
    }
    if (!entered) {
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.release = true;
        }
        gate.changed.notify_all();
        spawn_thread.join();
        sintra::detail::test_hooks::s_managed_child_post_spawn.store(
            nullptr, std::memory_order_release);
        s_post_spawn_gate = nullptr;
        return false;
    }

    std::atomic<bool> watchdog_released{false};
    std::thread watchdog([&]() {
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (!gate.changed.wait_for(lock, 5s, [&]() { return gate.release; })) {
            watchdog_released.store(true, std::memory_order_release);
            gate.release = true;
            lock.unlock();
            gate.changed.notify_all();
        }
    });

    const bool first_finalize = sintra::detail::finalize();
    const bool finalized_while_held = first_finalize &&
        !watchdog_released.load(std::memory_order_acquire);
    if (finalized_while_held) {
        std::fprintf(
            stderr,
            "A3A_SYNC_LIFETIME_RED finalize_succeeded_while_post_spawn_held=1\n");
        std::fflush(stderr);
        std::_Exit(1);
    }

    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.changed.notify_all();
    spawn_thread.join();
    watchdog.join();
    sintra::detail::test_hooks::s_managed_child_post_spawn.store(
        nullptr, std::memory_order_release);
    s_post_spawn_gate = nullptr;

    const bool finalized = first_finalize || settle_finalize();
    const bool valid = spawn_returned.load(std::memory_order_acquire) && finalized;
    if (valid) {
        std::fprintf(
            stdout,
            "A3A_SYNC_LIFETIME_GREEN finalize_succeeded_while_post_spawn_held=0\n");
    }
    return valid;
}

bool run_worker_rejection(
    const std::string& binary_path,
    const std::filesystem::path& outcome,
    const std::filesystem::path& unexpected_child)
{
    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_worker_flag, outcome.string(), unexpected_child.string()};
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    const bool outcome_written = wait_for_file(outcome, 10s);

    int nested_accepted = 1;
    int worker_finalized = 0;
    if (outcome_written) {
        std::ifstream in(outcome, std::ios::binary);
        in >> nested_accepted >> worker_finalized;
    }
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 15s);
    if (released.release_state != sintra::Managed_child_release_state::complete) {
        custody.terminate_until(std::chrono::steady_clock::now() + 15s);
    }
    return
        custody                           &&
        outcome_written                   &&
        nested_accepted    == 0           &&
        worker_finalized   == 1           &&
        released.release_state ==
            sintra::Managed_child_release_state::complete &&
        released.created_occurrences == 1 &&
        released.exited_occurrences  == 1 &&
        marker_absent(unexpected_child);
}

int run_worker(
    int argc,
    char* argv[],
    const std::filesystem::path& outcome,
    const std::filesystem::path& unexpected_child)
{
    sintra::init(argc, argv);
    sintra::Spawn_options nested;
    nested.binary_path = std::filesystem::absolute(argv[0]).string();
    nested.args = {k_unexpected_child_flag, unexpected_child.string()};
    nested.lifetime.enable_lifeline = false;
    const bool nested_accepted = static_cast<bool>(sintra::spawn_swarm_process(nested));
    const bool finalized = settle_finalize();
    std::ofstream out(outcome, std::ios::binary | std::ios::trunc);
    out << (nested_accepted ? 1 : 0) << ' ' << (finalized ? 1 : 0) << '\n';
    out.flush();
    return !nested_accepted && finalized && out.good() ? 0 : 3;
}

} // namespace

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == k_unexpected_child_flag && i + 1 < argc) {
            std::ofstream(argv[i + 1], std::ios::binary | std::ios::trunc)
                << "unexpected child\n";
            return 2;
        }
        if (arg == k_owned_child_flag && i + 1 < argc) {
            if (!write_child_identity(argv[i + 1])) {
                return 3;
            }
            std::this_thread::sleep_for(30s);
            return 0;
        }
        if (arg == k_worker_flag && i + 2 < argc) {
            return run_worker(argc, argv, argv[i + 1], argv[i + 2]);
        }
    }

    const auto scratch = sintra::test::unique_scratch_directory(
        "managed_child_spawn_admission");
    const auto malformed_marker = scratch / "malformed_child.marker";
    const auto pre_create_marker = scratch / "pre_create_child.marker";
    const auto post_native_marker = scratch / "post_native_child.marker";
    const auto missing_binary = scratch / "managed_child_missing_binary";
    const auto worker_outcome = scratch / "worker.outcome";
    const auto worker_child = scratch / "worker_nested_child.marker";
    const std::string binary_path = std::filesystem::absolute(argv[0]).string();

    sintra::init(argc, argv);
    const bool malformed_rejected = run_malformed_id_rejections(
        binary_path, malformed_marker);
    const bool pre_create_settled = run_pre_create_exception(
        binary_path, pre_create_marker);
    const bool post_native_settled = run_post_native_exception(
        binary_path, post_native_marker);
    const bool native_spawn_reported = run_native_spawn_failure(missing_binary);
    const bool worker_rejected = run_worker_rejection(
        binary_path, worker_outcome, worker_child);
    const bool finalized = run_sync_failure_finalize_fence(missing_binary);

    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);

    const bool valid = malformed_rejected && pre_create_settled &&
        post_native_settled && native_spawn_reported && worker_rejected && finalized;
    if (!valid) {
        std::fprintf(stderr,
            "MANAGED_CHILD_SPAWN_ADMISSION_INVALID malformed=%d pre_create=%d "
            "post_native=%d native_spawn=%d worker=%d finalized=%d\n",
            malformed_rejected ? 1 : 0,
            pre_create_settled ? 1 : 0,
            post_native_settled ? 1 : 0,
            native_spawn_reported ? 1 : 0,
            worker_rejected ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid ? 0 : 1;
}
