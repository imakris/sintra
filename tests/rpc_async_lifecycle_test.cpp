#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

// ponytail: test-only access for asserting Rpc_handle destructor cleanup.
#define private public
#include <sintra/sintra.h>
#undef private

#include "test_utils.h"

namespace {

constexpr std::string_view k_failure_prefix   = "rpc_async_lifecycle_test: ";
constexpr const char*      k_service_name     = "async lifecycle service";
constexpr const char*      k_ready_barrier    = "async-lifecycle-service-ready";
constexpr const char*      k_finished_barrier = "async-lifecycle-test-finished";
std::filesystem::path owner_events_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "owner_events.txt";
}

std::filesystem::path client_events_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "client_events.txt";
}

std::filesystem::path done_marker_path(
    const std::filesystem::path&   shared_dir,
    const std::string&             tag)
{
    return shared_dir / (tag + "_done.txt");
}

void touch_done_marker(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create marker file: " + path.string());
    }
    out << "done\n";
}

bool report_state_failure(std::string_view context, std::string_view message)
{
    std::fprintf(stderr,
        "%.*s%.*s %.*s\n",
        static_cast<int>(k_failure_prefix.size()),
        k_failure_prefix.data(),
        static_cast<int>(context.size()),
        context.data(),
        static_cast<int>(message.size()),
        message.data());
    return false;
}

bool wait_for_owner_event(const std::filesystem::path& shared_dir, std::string_view expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : sintra::test::read_lines(owner_events_path(shared_dir))) {
            if (event == expected) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

template <typename State>
bool expect_rpc_abandoned_and_detached(
    const std::shared_ptr<State>&  rpc_state,
    std::string_view              context)
{
    {
        std::lock_guard<std::mutex> lock(rpc_state->control.keep_waiting_mutex);
        if (rpc_state->control.keep_waiting) {
            return report_state_failure(context, "is still waiting");
        }
        if (!rpc_state->control.abandoned) {
            return report_state_failure(context, "is not marked abandoned");
        }
        if (!rpc_state->control.cleaned_up) {
            return report_state_failure(context, "is not cleaned up");
        }
    }

    {
        std::lock_guard<std::mutex> lock(sintra::s_outstanding_rpcs_mutex());
        if (sintra::s_outstanding_rpcs().find(&rpc_state->control) !=
            sintra::s_outstanding_rpcs().end())
        {
            return report_state_failure(context, "still has outstanding rpc control");
        }
    }

    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_return_handlers_mutex);
        if (sintra::s_mproc->m_active_return_handlers.find(rpc_state->function_instance_id) !=
            sintra::s_mproc->m_active_return_handlers.end())
        {
            return report_state_failure(context, "still has active return handler");
        }
    }

    return true;
}

template <typename Handle>
bool get_throws_runtime_error_not_cancelled(Handle& handle)
{
    try {
        (void)handle.get();
    }
    catch (const sintra::rpc_cancelled&) {
        return false;
    }
    catch (const std::runtime_error&) {
        return true;
    }
    catch (...) {
        return false;
    }

    return false;
}

template <typename Handle>
bool get_throws_cancelled(Handle& handle)
{
    try {
        (void)handle.get();
    }
    catch (const sintra::rpc_cancelled&) {
        return true;
    }
    catch (...) {
        return false;
    }

    return false;
}

template <typename Handle>
bool get_throws_runtime_error_message(Handle& handle, const std::string& expected_message)
{
    try {
        (void)handle.get();
    }
    catch (const sintra::rpc_cancelled&) {
        return false;
    }
    catch (const std::runtime_error& ex) {
        return ex.what() == expected_message;
    }
    catch (...) {
        return false;
    }

    return false;
}

void print_event_mismatch(
    const char*                        label,
    const std::vector<std::string>&    expected,
    const std::vector<std::string>&    actual)
{
    std::fprintf(stderr, "%s mismatch\n", label);
    std::fprintf(stderr, "expected:\n");
    for (const auto& entry : expected) {
        std::fprintf(stderr, "  %s\n", entry.c_str());
    }
    std::fprintf(stderr, "actual:\n");
    for (const auto& entry : actual) {
        std::fprintf(stderr, "  %s\n", entry.c_str());
    }
}

struct Lifecycle_service : sintra::Derived_transceiver<Lifecycle_service>
{
    void unicast_marker(
        std::string    tag,
        std::string    shared_dir_str)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("unicast:") + tag);
        touch_done_marker(done_marker_path(shared_dir, "unicast_" + tag));
    }

    int immediate_reply(int value)
    {
        return value + 1;
    }

    int delayed_reply(
        int            value,
        int            delay_ms,
        std::string    tag,
        std::string    shared_dir_str)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("start:") + tag);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("finish:") + tag);
        touch_done_marker(done_marker_path(shared_dir, tag));
        return value;
    }

    int throwing_reply(
        std::string    message,
        std::string    tag,
        std::string    shared_dir_str)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("throw:") + tag);
        touch_done_marker(done_marker_path(shared_dir, tag));
        throw std::runtime_error(message);
    }

    SINTRA_UNICAST(unicast_marker)
    SINTRA_RPC(immediate_reply)
    SINTRA_RPC_STRICT(delayed_reply)
    SINTRA_RPC_STRICT(throwing_reply)
};

int process_owner()
{
    Lifecycle_service service;
    service.assign_name(k_service_name);

    sintra::barrier(k_ready_barrier);
    sintra::barrier(k_finished_barrier, "_sintra_all_processes");
    return 0;
}

int process_client()
{
    using namespace std::chrono_literals;

    static_assert(!std::is_copy_constructible_v<sintra::Rpc_handle<int>>);
    static_assert(!std::is_copy_assignable_v<sintra::Rpc_handle<int>>);

    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "rpc_async_lifecycle");
    const auto& shared_dir  = shared.path();
    const auto  client_path = client_events_path(shared_dir);

    sintra::barrier(k_ready_barrier);

    {
        Lifecycle_service local_service;
        bool rejected = false;

        try {
            auto handle = Lifecycle_service::rpc_async_immediate_reply(local_service.instance_id(), 17);
            (void)handle;
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }

        if (!rejected) {
            sintra::test::append_line_or_throw(client_path, "local_async_non_strict:unexpected");
            return 1;
        }

        sintra::test::append_line_or_throw(client_path, "local_async_non_strict:rejected");
    }

    {
        Lifecycle_service local_service;
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            local_service.instance_id(),
            88,
            80,
            "local_strict",
            shared_dir.string());

        if (handle.get_until(std::chrono::steady_clock::now() + 2s) != 88) {
            sintra::test::append_line_or_throw(client_path, "local_async_strict_get_until:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "local_async_strict_get_until:returned");

        if (handle.get() != 88) {
            sintra::test::append_line_or_throw(client_path, "local_async_strict_get:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "local_async_strict_get:returned");
    }

    {
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            k_service_name,
            303,
            1,
            "fast_complete",
            shared_dir.string());

        std::this_thread::sleep_for(40ms);

        if (handle.get_until(std::chrono::steady_clock::now() + 100ms) != 303) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_get_until:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_get_until:returned");

        if (sintra::s_mproc->unblock_rpc() != 0) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_unblock:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_unblock:clean");

        if (handle.get() != 303) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_get:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_get:returned");
    }

    {
        auto handle = Lifecycle_service::rpc_async_throwing_reply(
            k_service_name,
            "remote exception observed",
            "remote_exception",
            shared_dir.string());

        try {
            (void)handle.get_until(std::chrono::steady_clock::now() + 2s);
            sintra::test::append_line_or_throw(client_path, "remote_exception_get_until:unexpected");
            return 1;
        }
        catch (const std::runtime_error& ex) {
            if (std::string(ex.what()) != "remote exception observed") {
                sintra::test::append_line_or_throw(client_path, "remote_exception_get_until:unexpected");
                return 1;
            }
        }
        catch (...) {
            sintra::test::append_line_or_throw(client_path, "remote_exception_get_until:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "remote_exception_get_until:threw");

        if (!get_throws_runtime_error_message(handle, "remote exception observed")) {
            sintra::test::append_line_or_throw(client_path, "remote_exception_get:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "remote_exception_get:threw");
    }

    {
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            k_service_name,
            101,
            250,
            "abandon",
            shared_dir.string());

        try {
            (void)handle.get_until(std::chrono::steady_clock::now() + 40ms);
            sintra::test::append_line_or_throw(client_path, "abandon_get_until:unexpected");
            return 1;
        }
        catch (const sintra::rpc_timeout&) {
        }
        catch (...) {
            sintra::test::append_line_or_throw(client_path, "abandon_get_until:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_get_until:timed_out");

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "abandon"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "abandon_late_reply:missing");
            return 1;
        }

        if (!get_throws_runtime_error_not_cancelled(handle)) {
            sintra::test::append_line_or_throw(client_path, "abandon_get:returned");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_get:threw");
    }

    {
        std::shared_ptr<sintra::Rpc_state<int>> rpc_state;

        {
            auto handle = Lifecycle_service::rpc_async_delayed_reply(
                k_service_name,
                404,
                250,
                "scope_abandon",
                shared_dir.string());
            rpc_state = handle.m_state;

            if (!wait_for_owner_event(shared_dir, "start:scope_abandon")) {
                sintra::test::append_line_or_throw(client_path, "scope_abandon_start:missing");
                return 1;
            }
        }

        if (!expect_rpc_abandoned_and_detached(rpc_state, "scope_abandon_drop")) {
            sintra::test::append_line_or_throw(client_path, "scope_abandon_drop:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "scope_abandon_drop:destroyed");

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "scope_abandon"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "scope_abandon_late_reply:missing");
            return 1;
        }

        if (sintra::s_mproc->unblock_rpc() != 0) {
            sintra::test::append_line_or_throw(client_path, "scope_abandon_cleanup:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "scope_abandon_cleanup:clean");
    }

    {
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            k_service_name,
            202,
            350,
            "cancel",
            shared_dir.string());

        size_t     cancelled_count = 0;
        const auto cancel_deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < cancel_deadline) {
            cancelled_count = sintra::s_mproc->unblock_rpc();
            if (cancelled_count != 0) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }

        if (cancelled_count != 1) {
            sintra::test::append_line_or_throw(client_path, "cancel_unblock:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_unblock:observed");

        try {
            (void)handle.get_until(std::chrono::steady_clock::now() + 200ms);
            sintra::test::append_line_or_throw(client_path, "cancel_get_until:unexpected");
            return 1;
        }
        catch (const sintra::rpc_cancelled&) {
        }
        catch (...) {
            sintra::test::append_line_or_throw(client_path, "cancel_get_until:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_get_until:threw");

        if (!get_throws_cancelled(handle)) {
            sintra::test::append_line_or_throw(client_path, "cancel_get:returned");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_get:threw");

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "cancel"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "cancel_late_reply:missing");
            return 1;
        }
    }

    {
        Lifecycle_service::rpc_unicast_marker(
            k_service_name,
            "remote_unicast",
            shared_dir.string());
        sintra::test::append_line_or_throw(client_path, "unicast_call:returned");

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "unicast_remote_unicast"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "unicast_delivery:missing");
            return 1;
        }

        sintra::test::append_line_or_throw(client_path, "unicast_delivery:observed");
    }

    sintra::barrier(k_finished_barrier, "_sintra_all_processes");
    return 0;
}

int verify_results(const std::filesystem::path& shared_dir)
{
    const auto owner_events  = sintra::test::read_lines(owner_events_path(shared_dir));
    const auto client_events = sintra::test::read_lines(client_events_path(shared_dir));

    const std::vector<std::string> expected_owner_events = {
        "start:local_strict",
        "finish:local_strict",
        "start:fast_complete",
        "finish:fast_complete",
        "throw:remote_exception",
        "start:abandon",
        "finish:abandon",
        "start:scope_abandon",
        "finish:scope_abandon",
        "start:cancel",
        "finish:cancel",
        "unicast:remote_unicast",
    };

    const std::vector<std::string> expected_client_events = {
        "local_async_non_strict:rejected",
        "local_async_strict_get_until:returned",
        "local_async_strict_get:returned",
        "fast_complete_get_until:returned",
        "fast_complete_unblock:clean",
        "fast_complete_get:returned",
        "remote_exception_get_until:threw",
        "remote_exception_get:threw",
        "abandon_get_until:timed_out",
        "abandon_get:threw",
        "scope_abandon_drop:destroyed",
        "scope_abandon_cleanup:clean",
        "cancel_unblock:observed",
        "cancel_get_until:threw",
        "cancel_get:threw",
        "unicast_call:returned",
        "unicast_delivery:observed",
    };

    if (owner_events != expected_owner_events) {
        print_event_mismatch("owner_events", expected_owner_events, owner_events);
        return 1;
    }

    if (client_events != expected_client_events) {
        print_event_mismatch("client_events", expected_client_events, client_events);
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "rpc_async_lifecycle",
        {process_owner,
         process_client},
        [](const std::filesystem::path&) {
            sintra::barrier(k_finished_barrier, "_sintra_all_processes");
            return 0;
        },
        verify_results);
}
