// An explicitly detached external member is admitted only with exact
// coordinator identity. The first collective-shutdown entry notifies it
// before taking a lifecycle-barrier snapshot, and the member can leave on its
// control thread without joining the collective shutdown.

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "exact_child_test_support.h"
#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::string_view k_member_flag = "--detached-external-member";
constexpr auto k_timeout = 20s;
constexpr auto k_poll = 10ms;

std::atomic<unsigned> s_hook_sequence{0};
std::atomic<unsigned> s_notice_sequence{0};
std::atomic<unsigned> s_snapshot_sequence{0};
std::atomic<unsigned> s_notice_count{0};
std::atomic<unsigned> s_snapshot_count{0};
std::atomic<bool> s_order_violation{false};

fs::path marker(const fs::path& directory, const char* name)
{
    return directory / name;
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(k_poll);
    }
    return true;
}

void observe_collective_stage(const char* stage)
{
    if (!stage) {
        return;
    }
    const std::string_view observed(stage);
    if (observed ==
        sintra::detail::test_hooks::k_stage_collective_detached_notice_sent)
    {
        const auto sequence =
            s_hook_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        s_notice_count.fetch_add(1, std::memory_order_release);
        unsigned unset = 0;
        (void)s_notice_sequence.compare_exchange_strong(
            unset, sequence, std::memory_order_acq_rel);
        return;
    }
    if (observed ==
        sintra::detail::test_hooks::k_stage_collective_before_barrier_snapshot)
    {
        const auto sequence =
            s_hook_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        s_snapshot_count.fetch_add(1, std::memory_order_release);
        unsigned unset = 0;
        (void)s_snapshot_sequence.compare_exchange_strong(
            unset, sequence, std::memory_order_acq_rel);
        if (s_notice_sequence.load(std::memory_order_acquire) == 0) {
            s_order_violation.store(true, std::memory_order_release);
        }
    }
}

int run_member(int argc, char* argv[], const fs::path& directory)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    std::atomic<unsigned> departures{0};
    std::atomic<bool> callback_thread_valid{true};
    std::atomic<sintra::member_lifecycle_event::departure_cause> cause{
        sintra::member_lifecycle_event::departure_cause::NONE};
    const auto control_thread = std::this_thread::get_id();
    if (!sintra::set_member_lifecycle_handler(
            [&](const sintra::member_lifecycle_event& event) {
                if (event.why != sintra::member_lifecycle_event::kind::
                        COORDINATOR_DEPARTED)
                {
                    return;
                }
                if (std::this_thread::get_id() == control_thread) {
                    callback_thread_valid.store(false, std::memory_order_release);
                }
                cause.store(event.cause, std::memory_order_release);
                departures.fetch_add(1, std::memory_order_release);
            }))
    {
        return 3;
    }

    if (sintra::s_mproc->m_member_lifetime_role.load(
            std::memory_order_acquire) !=
            sintra::detail::Member_lifetime_role::DETACHED ||
        !sintra::test::managed_child::write_child_identity(
            marker(directory, "member.ready")))
    {
        return 4;
    }

    if (!wait_until([&] {
            return departures.load(std::memory_order_acquire) == 1;
        }, k_timeout))
    {
        return 5;
    }
    std::this_thread::sleep_for(100ms);
    if (departures.load(std::memory_order_acquire) != 1 ||
        !callback_thread_valid.load(std::memory_order_acquire) ||
        cause.load(std::memory_order_acquire) !=
            sintra::member_lifecycle_event::departure_cause::COLLECTIVE_SHUTDOWN ||
        !sintra::leave() ||
        !sintra::test::managed_child::write_complete_file(
            marker(directory, "member.dormant"), "1\n") ||
        !sintra::test::wait_for_file(
            marker(directory, "member.finish"), k_timeout, k_poll))
    {
        return 6;
    }
    return 0;
}

bool launch_member(
    const std::string& binary_path,
    const sintra::External_process_invitation& invitation,
    sintra::test::Exact_child& child)
{
    std::vector<std::string> args{binary_path, std::string(k_member_flag)};
    auto invitation_args = invitation.sintra_args();
    args.insert(args.end(), invitation_args.begin(), invitation_args.end());
    sintra::C_string_vector cargs(args);
    return child.spawn(binary_path.c_str(), cargs.v());
}

int run_coordinator(
    int argc,
    char* argv[],
    const std::string& binary_path,
    const fs::path& directory)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    sintra::External_process_invitation_options detached_options;
    detached_options.detached = true;
    detached_options.timeout = k_timeout;

    const auto exact_start_stamp = sintra::s_mproc->m_process_start_stamp;
    sintra::s_mproc->m_process_start_stamp = 0;
    const bool missing_identity_rejected =
        !sintra::create_external_process_invitation(detached_options);
    sintra::s_mproc->m_process_start_stamp = exact_start_stamp + 1;
    const bool mismatched_identity_rejected =
        !sintra::create_external_process_invitation(detached_options);
    sintra::s_mproc->m_process_start_stamp = exact_start_stamp;

    const auto invitation =
        sintra::create_external_process_invitation(detached_options);
    sintra::test::Exact_child member(5s);
    const bool launched = invitation &&
        launch_member(binary_path, invitation, member);
    const auto identity = launched
        ? sintra::test::managed_child::wait_for_child_identity(
            marker(directory, "member.ready"), k_timeout, k_poll)
        : std::nullopt;

    s_hook_sequence.store(0, std::memory_order_release);
    s_notice_sequence.store(0, std::memory_order_release);
    s_snapshot_sequence.store(0, std::memory_order_release);
    s_notice_count.store(0, std::memory_order_release);
    s_snapshot_count.store(0, std::memory_order_release);
    s_order_violation.store(false, std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &observe_collective_stage, std::memory_order_release);
    bool shutdown_complete = false;
    if (identity) {
        try {
            shutdown_complete = sintra::shutdown();
        }
        catch (...) {
        }
    }
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr, std::memory_order_release);

    const bool dormant = identity && sintra::test::wait_for_file(
        marker(directory, "member.dormant"), k_timeout, k_poll);
    const bool survived = identity && dormant &&
        member.poll() == sintra::test::Exact_child_state::running;
    const bool finish_sent = sintra::test::managed_child::write_complete_file(
        marker(directory, "member.finish"), "1\n");
    const bool exited = wait_until([&] {
        return member.poll() == sintra::test::Exact_child_state::exited;
    }, k_timeout);
    std::string settlement;
    const bool clean_exit = exited && member.exited_with_code(0) &&
        member.settle_observed_exit(settlement);

    const auto notice_sequence =
        s_notice_sequence.load(std::memory_order_acquire);
    const auto snapshot_sequence =
        s_snapshot_sequence.load(std::memory_order_acquire);
    const bool causal_order =
        s_notice_count.load(std::memory_order_acquire) == 1 &&
        s_snapshot_count.load(std::memory_order_acquire) > 0 &&
        notice_sequence > 0 && snapshot_sequence > notice_sequence &&
        !s_order_violation.load(std::memory_order_acquire);

    if (!missing_identity_rejected || !mismatched_identity_rejected ||
        !invitation || !launched || !identity || !shutdown_complete ||
        !causal_order || !dormant || !survived || !finish_sent || !clean_exit)
    {
        std::fprintf(stderr,
            "DETACHED_EXTERNAL_COLLECTIVE_INVALID missing=%d mismatch=%d "
            "invitation=%d launched=%d ready=%d shutdown=%d notice=%u/%u "
            "snapshot=%u/%u violation=%d dormant=%d survived=%d finish=%d "
            "exit=%d detail=%s\n",
            missing_identity_rejected ? 1 : 0,
            mismatched_identity_rejected ? 1 : 0,
            invitation ? 1 : 0,
            launched ? 1 : 0,
            identity ? 1 : 0,
            shutdown_complete ? 1 : 0,
            s_notice_count.load(), notice_sequence,
            s_snapshot_count.load(), snapshot_sequence,
            s_order_violation.load() ? 1 : 0,
            dormant ? 1 : 0,
            survived ? 1 : 0,
            finish_sent ? 1 : 0,
            clean_exit ? 1 : 0,
            settlement.c_str());
        if (sintra::s_mproc) {
            (void)sintra::detail::finalize();
        }
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_DETACHED_EXTERNAL_COLLECTIVE_DIR",
        "detached_external_collective");
    if (sintra::test::has_argv_flag(argc, argv, k_member_flag)) {
        return run_member(argc, argv, shared.path());
    }
    const auto binary_path = sintra::test::get_binary_path(argc, argv);
    return binary_path.empty()
        ? 2
        : run_coordinator(argc, argv, binary_path, shared.path());
}
