// A detached member observes exact coordinator death through an owned native
// wait, delivers one callback, and leaves from its control thread.

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "exact_child_test_support.h"
#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::Exact_child;
using sintra::test::Exact_child_state;
using sintra::test::managed_child::child_identity_t;
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::read_child_identity;
using sintra::test::managed_child::terminate_process_by_pid;
using sintra::test::managed_child::wait_for_exact_process_absence;
using sintra::test::managed_child::write_child_identity;
using sintra::test::managed_child::write_complete_file;

constexpr auto k_timeout = 20s;
constexpr auto k_poll = 10ms;
constexpr std::string_view k_coordinator_flag = "--exact-watch-coordinator";
constexpr std::string_view k_member_flag = "--exact-watch-member";

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

std::shared_ptr<sintra::detail::Managed_child_custody_record>
custody_record_for(sintra::instance_id_type process_iid)
{
    std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
    const auto found =
        sintra::s_mproc->m_child_custody_by_process.find(process_iid);
    return found == sintra::s_mproc->m_child_custody_by_process.end()
        ? nullptr
        : found->second.custody.lock();
}

int run_member(int argc, char* argv[], const fs::path& directory)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    std::atomic<unsigned> lifeline_events{0};
    std::atomic<unsigned> departure_events{0};
    std::atomic<bool> callback_thread_valid{true};
    const auto control_thread = std::this_thread::get_id();
    if (!sintra::s_mproc->set_member_lifecycle_handler(
            [&](const sintra::detail::Member_lifecycle_event& event) {
                if (std::this_thread::get_id() == control_thread) {
                    callback_thread_valid.store(false, std::memory_order_release);
                }
                if (event.kind == sintra::detail::Member_lifecycle_event_kind::
                        LIFELINE_RELEASED)
                {
                    lifeline_events.fetch_add(1, std::memory_order_release);
                }
                else {
                    departure_events.fetch_add(1, std::memory_order_release);
                }
            }))
    {
        return 3;
    }

    sintra::Coordinator::rpc_mark_initialization_complete(
        sintra::s_coord_id, sintra::s_mproc_id);
    if (!write_child_identity(marker(directory, "member.ready"))) {
        return 4;
    }

    if (!wait_until([&] {
            return lifeline_events.load(std::memory_order_acquire) == 1;
        }, k_timeout) ||
        !write_complete_file(marker(directory, "member.released"), "1\n"))
    {
        return 5;
    }
    if (!wait_until([&] {
            return departure_events.load(std::memory_order_acquire) == 1;
        }, k_timeout))
    {
        return 6;
    }
    std::this_thread::sleep_for(100ms);
    if (lifeline_events.load(std::memory_order_acquire) != 1 ||
        departure_events.load(std::memory_order_acquire) != 1 ||
        !callback_thread_valid.load(std::memory_order_acquire) ||
        sintra::s_mproc->m_coordinator_departure_cause.load(
            std::memory_order_acquire) !=
            sintra::detail::Coordinator_departure_cause::EXACT_OS_WATCH)
    {
        return 7;
    }

    if (!sintra::leave() ||
        !write_complete_file(marker(directory, "member.dormant"), "1\n") ||
        !sintra::test::wait_for_file(
            marker(directory, "member.finish"), k_timeout, k_poll))
    {
        return 8;
    }
    return 0;
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

    const auto member_iid = sintra::make_process_instance_id();
    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {std::string(k_member_flag)};
    options.process_instance_id = member_iid;
    options.lifetime.enable_lifeline = true;
    auto custody = sintra::spawn_swarm_process(options);
    if (!write_complete_file(
            marker(directory, "swarm.path"), sintra::s_mproc->m_directory))
    {
        return 3;
    }
    const bool member_ready = sintra::test::wait_for_file(
        marker(directory, "member.ready"), k_timeout, k_poll);
    const auto record = custody_record_for(member_iid);
    const auto detached = record
        ? sintra::s_mproc->detach_child_custody_until(
            record, std::chrono::steady_clock::now() + k_timeout)
        : sintra::detail::Managed_child_detach_result::NOT_STARTED;
    if (!member_ready ||
        detached != sintra::detail::Managed_child_detach_result::DISOWNED ||
        !sintra::test::wait_for_file(
            marker(directory, "member.released"), k_timeout, k_poll))
    {
        return 4;
    }

    std::_Exit(73);
}

int run_supervisor(const std::string& binary_path, const fs::path& directory)
{
    const std::string coordinator_flag(k_coordinator_flag);
    const char* args[] = {
        binary_path.c_str(),
        coordinator_flag.c_str(),
        nullptr,
    };
    Exact_child coordinator(5s);
    if (!coordinator.spawn(binary_path.c_str(), args)) {
        return 2;
    }

    std::optional<child_identity_t> member;
    const bool member_ready = wait_until([&] {
        member = read_child_identity(marker(directory, "member.ready"));
        return member.has_value();
    }, k_timeout);
    const bool coordinator_exited = wait_until([&] {
        return coordinator.poll() == Exact_child_state::exited;
    }, k_timeout);
    std::string diagnostic;
    const bool exact_exit = coordinator_exited &&
        coordinator.settle_observed_exit(diagnostic) &&
        coordinator.exited_with_code(73);
    const bool dormant = sintra::test::wait_for_file(
        marker(directory, "member.dormant"), k_timeout, k_poll);
    const bool member_survived = member && exact_process_is_live(*member);

    fs::path swarm_directory;
    {
        std::ifstream swarm_path(marker(directory, "swarm.path"));
        std::string value;
        std::getline(swarm_path, value);
        swarm_directory = value;
    }
    std::error_code remove_error;
    const bool resources_removable = member && !swarm_directory.empty()
        ? (fs::remove_all(swarm_directory, remove_error), !remove_error)
        : false;
    const bool finish_sent = write_complete_file(
        marker(directory, "member.finish"), "1\n");
    const bool member_exited = member && wait_for_exact_process_absence(
        *member, k_timeout, k_poll);
    if (!member_exited && member && exact_process_is_live(*member)) {
        (void)terminate_process_by_pid(member->pid, 74);
    }

    if (!member_ready || !exact_exit || !dormant || !member_survived ||
        !resources_removable || !finish_sent || !member_exited)
    {
        std::fprintf(stderr,
            "DETACHED_MEMBER_EXACT_WATCH_INVALID ready=%d coord=%d dormant=%d "
            "survived=%d removable=%d finish=%d exited=%d detail=%s\n",
            member_ready ? 1 : 0,
            exact_exit ? 1 : 0,
            dormant ? 1 : 0,
            member_survived ? 1 : 0,
            resources_removable ? 1 : 0,
            finish_sent ? 1 : 0,
            member_exited ? 1 : 0,
            diagnostic.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_DETACHED_MEMBER_WATCH_DIR",
        "detached_member_exact_watch");
    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    if (sintra::test::has_argv_flag(argc, argv, k_member_flag)) {
        return run_member(argc, argv, shared.path());
    }
    if (sintra::test::has_argv_flag(argc, argv, k_coordinator_flag)) {
        return run_coordinator(argc, argv, binary_path, shared.path());
    }
    return run_supervisor(binary_path, shared.path());
}
