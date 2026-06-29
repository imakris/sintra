#include <atomic>
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
#include <vector>

// ponytail: test-only seam for the get_until timeout/abandon race; no
// production hook exists at that exact boundary.
#define private public
#include <sintra/sintra.h>
#undef private

#include "test_utils.h"

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_prefix          = "rpc_bounded_result_test: ";
constexpr const char*      k_service_name    = "rpc bounded result service";
constexpr const char*      k_ready_barrier   = "rpc-bounded-result-ready";
constexpr const char*      k_done_barrier    = "rpc-bounded-result-done";
constexpr int              k_race_value      = 731;
constexpr int              k_remote_value    = 219;
constexpr int              k_local_value     = 64;
constexpr const char*      k_logic_message   = "bounded logic identity";
constexpr const char*      k_unavail_message = "bounded unavailable identity";

std::filesystem::path marker_path(
    const std::filesystem::path&   shared_dir,
    const std::string&             tag)
{
    return shared_dir / (tag + ".marker");
}

std::filesystem::path client_events_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "client_events.txt";
}

void write_marker(const std::filesystem::path& shared_dir, const std::string& tag)
{
    std::ofstream out(marker_path(shared_dir, tag), std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write marker: " + tag);
    }
    out << tag << '\n';
}

bool wait_for_marker(const std::filesystem::path& shared_dir, const std::string& tag)
{
    return sintra::test::wait_for_file(marker_path(shared_dir, tag), 2s, 5ms);
}

void append_client_event(const std::filesystem::path& shared_dir, std::string_view event)
{
    sintra::test::append_line_or_throw(client_events_path(shared_dir), event);
}

template <typename T>
bool expect_equal(const T& actual, const T& expected, std::string_view context)
{
    if (actual == expected) {
        return true;
    }

    std::fprintf(stderr,
        "%.*s%.*s mismatch\n",
        static_cast<int>(k_prefix.size()),
        k_prefix.data(),
        static_cast<int>(context.size()),
        context.data());
    return false;
}

bool expect_marker(const std::filesystem::path& shared_dir, const std::string& tag)
{
    if (wait_for_marker(shared_dir, tag)) {
        return true;
    }

    std::fprintf(stderr, "%.*smissing marker: %s\n",
        static_cast<int>(k_prefix.size()),
        k_prefix.data(),
        tag.c_str());
    return false;
}

template <typename Handle>
bool expect_rpc_detached(const Handle& handle, std::string_view context)
{
    const auto rpc_state            = handle.m_state;
    const auto function_instance_id = rpc_state->function_instance_id;

    {
        std::lock_guard<std::mutex> lock(sintra::s_outstanding_rpcs_mutex());
        if (sintra::s_outstanding_rpcs().find(&rpc_state->control) !=
            sintra::s_outstanding_rpcs().end())
        {
            std::fprintf(stderr, "%.*s%.*s still has outstanding rpc control\n",
                static_cast<int>(k_prefix.size()),
                k_prefix.data(),
                static_cast<int>(context.size()),
                context.data());
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_return_handlers_mutex);
        if (sintra::s_mproc->m_active_return_handlers.find(function_instance_id) !=
            sintra::s_mproc->m_active_return_handlers.end())
        {
            std::fprintf(stderr, "%.*s%.*s still has active return handler\n",
                static_cast<int>(k_prefix.size()),
                k_prefix.data(),
                static_cast<int>(context.size()),
                context.data());
            return false;
        }
    }

    return true;
}

struct Bounded_service : sintra::Derived_transceiver<Bounded_service>
{
    int direct_value(int value)
    {
        return value;
    }

    int strict_value(int value)
    {
        return value;
    }

    void strict_void(std::string shared_dir_str, std::string tag)
    {
        write_marker(std::filesystem::path(shared_dir_str), tag);
    }

    int delayed_value(
        int            value,
        int            delay_ms,
        std::string    shared_dir_str,
        std::string    tag)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        write_marker(shared_dir, tag + "_started");
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        write_marker(shared_dir, tag + "_finished");
        return value;
    }

    void delayed_void(
        int            delay_ms,
        std::string    shared_dir_str,
        std::string    tag)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        write_marker(shared_dir, tag + "_started");
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        write_marker(shared_dir, tag + "_finished");
    }

    int throw_logic(std::string message)
    {
        throw std::logic_error(message);
    }

    int throw_unavailable(std::string message)
    {
        throw sintra::rpc_unavailable(message);
    }

    SINTRA_RPC(direct_value)
    SINTRA_RPC_STRICT(strict_value)
    SINTRA_RPC_STRICT(strict_void)
    SINTRA_RPC_STRICT(delayed_value)
    SINTRA_RPC_STRICT(delayed_void)
    SINTRA_RPC_STRICT(throw_logic)
    SINTRA_RPC_STRICT(throw_unavailable)
};

sintra::Rpc_state<int>* g_get_until_deadline_state = nullptr;
std::atomic<bool>       g_get_until_deadline_hook_completed{false};

void complete_get_until_deadline_race(const char* stage)
{
    if (std::string_view(stage) !=
        sintra::detail::test_hooks::k_stage_rpc_get_until_deadline_before_abandon)
    {
        return;
    }

    auto* state = g_get_until_deadline_state;
    std::lock_guard<std::mutex> lock(state->control.keep_waiting_mutex);
    state->return_body.value     = k_race_value;
    state->control.keep_waiting  = false;
    g_get_until_deadline_hook_completed.store(true, std::memory_order_release);
    state->control.keep_waiting_condition.notify_all();
}

bool get_until_preserves_completion_when_abandon_loses()
{
    sintra::Rpc_handle<int> handle;
    handle.m_state = std::make_shared<sintra::Rpc_state<int>>();

    auto clear_hook = [] {
        sintra::detail::test_hooks::s_rpc_get_until_stage.store(
            nullptr,
            std::memory_order_release);
        g_get_until_deadline_state = nullptr;
    };

    g_get_until_deadline_state = handle.m_state.get();
    g_get_until_deadline_hook_completed.store(false, std::memory_order_release);
    sintra::detail::test_hooks::s_rpc_get_until_stage.store(
        &complete_get_until_deadline_race,
        std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() - 1ms;

    try {
        const int value = handle.get_until(deadline);
        clear_hook();

        if (!g_get_until_deadline_hook_completed.load(std::memory_order_acquire)) {
            std::fprintf(stderr,
                "%.*sget_until deadline hook was not reached\n",
                static_cast<int>(k_prefix.size()),
                k_prefix.data());
            return false;
        }

        return expect_equal(value, k_race_value, "abandon-loses get_until completion");
    }
    catch (const std::exception& ex) {
        clear_hook();
        std::fprintf(stderr,
            "%.*sget_until abandon-loses path threw unexpected exception: %s\n",
            static_cast<int>(k_prefix.size()),
            k_prefix.data(),
            ex.what());
        return false;
    }
    catch (...) {
        clear_hook();
        std::fprintf(stderr,
            "%.*sget_until abandon-loses path threw unknown exception\n",
            static_cast<int>(k_prefix.size()),
            k_prefix.data());
        return false;
    }
}

bool test_same_process_async(const std::filesystem::path& shared_dir)
{
    Bounded_service local_service;

    bool rejected = false;
    try {
        auto handle = Bounded_service::rpc_async_direct_value(local_service.instance_id(), 7);
        (void)handle;
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }

    if (!rejected) {
        return false;
    }
    append_client_event(shared_dir, "same_process_non_strict_rejected");

    auto value_handle = Bounded_service::rpc_async_strict_value(
        local_service.instance_id(),
        k_local_value);
    const int via_get_until = value_handle.get_until(std::chrono::steady_clock::now() + 1s);
    const int via_get       = value_handle.get();
    if (!expect_equal(via_get_until, k_local_value, "completed non-void get_until") ||
        !expect_equal(via_get,       k_local_value, "completed non-void get"))
    {
        return false;
    }
    append_client_event(shared_dir, "same_process_strict_non_void_returned");

    auto void_handle = Bounded_service::rpc_async_strict_void(
        local_service.instance_id(),
        shared_dir.string(),
        "same_process_void");
    void_handle.get_until(std::chrono::steady_clock::now() + 1s);
    void_handle.get();
    if (!expect_marker(shared_dir, "same_process_void")) {
        return false;
    }
    append_client_event(shared_dir, "same_process_strict_void_returned");

    return true;
}

bool test_remote_success_and_exceptions(const std::filesystem::path& shared_dir)
{
    auto remote_value = Bounded_service::rpc_async_strict_value(k_service_name, k_remote_value);
    if (remote_value.get_until(std::chrono::steady_clock::now() + 2s) != k_remote_value) {
        return false;
    }
    append_client_event(shared_dir, "remote_non_void_returned");

    auto remote_void = Bounded_service::rpc_async_strict_void(
        k_service_name,
        shared_dir.string(),
        "remote_void");
    remote_void.get_until(std::chrono::steady_clock::now() + 2s);
    if (!expect_marker(shared_dir, "remote_void")) {
        return false;
    }
    append_client_event(shared_dir, "remote_void_returned");

    auto logic = Bounded_service::rpc_async_throw_logic(k_service_name, k_logic_message);
    try {
        (void)logic.get_until(std::chrono::steady_clock::now() + 2s);
        return false;
    }
    catch (const std::logic_error& ex) {
        if (std::string(ex.what()) != k_logic_message) {
            return false;
        }
    }
    catch (...) {
        return false;
    }
    append_client_event(shared_dir, "remote_logic_identity_preserved");

    auto unavailable = Bounded_service::rpc_async_throw_unavailable(
        k_service_name,
        k_unavail_message);
    try {
        (void)unavailable.get_until(std::chrono::steady_clock::now() + 2s);
        return false;
    }
    catch (const sintra::rpc_unavailable& ex) {
        if (std::string(ex.what()) != k_unavail_message) {
            return false;
        }
    }
    catch (...) {
        return false;
    }
    append_client_event(shared_dir, "remote_unavailable_identity_preserved");

    return true;
}

bool expect_timeout_cleanup(
    sintra::Rpc_handle<int>&       handle,
    const std::filesystem::path&   shared_dir,
    const std::string&             tag)
{
    if (!expect_marker(shared_dir, tag + "_started")) {
        return false;
    }

    try {
        (void)handle.get_until(std::chrono::steady_clock::now() + 25ms);
        return false;
    }
    catch (const sintra::rpc_timeout&) {
    }
    catch (...) {
        return false;
    }

    if (!expect_rpc_detached(handle, tag)) {
        return false;
    }

    if (!expect_marker(shared_dir, tag + "_finished")) {
        return false;
    }

    return sintra::s_mproc->unblock_rpc() == 0;
}

bool expect_timeout_cleanup(
    sintra::Rpc_handle<void>&      handle,
    const std::filesystem::path&   shared_dir,
    const std::string&             tag)
{
    if (!expect_marker(shared_dir, tag + "_started")) {
        return false;
    }

    try {
        handle.get_until(std::chrono::steady_clock::now() + 25ms);
        return false;
    }
    catch (const sintra::rpc_timeout&) {
    }
    catch (...) {
        return false;
    }

    if (!expect_rpc_detached(handle, tag)) {
        return false;
    }

    if (!expect_marker(shared_dir, tag + "_finished")) {
        return false;
    }

    return sintra::s_mproc->unblock_rpc() == 0;
}

bool test_timeouts_and_cancel(const std::filesystem::path& shared_dir)
{
    auto timeout_value = Bounded_service::rpc_async_delayed_value(
        k_service_name,
        901,
        250,
        shared_dir.string(),
        "timeout_value");
    if (!expect_timeout_cleanup(timeout_value, shared_dir, "timeout_value")) {
        return false;
    }
    append_client_event(shared_dir, "timeout_non_void_abandoned");

    auto timeout_void = Bounded_service::rpc_async_delayed_void(
        k_service_name,
        250,
        shared_dir.string(),
        "timeout_void");
    if (!expect_timeout_cleanup(timeout_void, shared_dir, "timeout_void")) {
        return false;
    }
    append_client_event(shared_dir, "timeout_void_abandoned");

    auto cancelled = Bounded_service::rpc_async_delayed_value(
        k_service_name,
        333,
        250,
        shared_dir.string(),
        "cancelled");
    if (!expect_marker(shared_dir, "cancelled_started")) {
        return false;
    }

    size_t cancelled_count = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        cancelled_count = sintra::s_mproc->unblock_rpc();
        if (cancelled_count != 0) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    if (cancelled_count != 1) {
        return false;
    }

    try {
        (void)cancelled.get_until(std::chrono::steady_clock::now() + 2s);
        return false;
    }
    catch (const sintra::rpc_cancelled&) {
    }
    catch (...) {
        return false;
    }

    if (!expect_marker(shared_dir, "cancelled_finished")) {
        return false;
    }

    append_client_event(shared_dir, "cancelled_identity_preserved");
    return true;
}

int process_owner()
{
    Bounded_service service;
    service.assign_name(k_service_name);

    sintra::barrier(k_ready_barrier);
    sintra::barrier(k_done_barrier, "_sintra_all_processes");
    return 0;
}

int process_client()
{
    const sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR",
        "rpc_bounded_result");
    const auto shared_dir = shared.path();

    sintra::barrier(k_ready_barrier);

    bool ok = true;
    ok = test_same_process_async(shared_dir) && ok;
    ok = ok && test_remote_success_and_exceptions(shared_dir);
    ok = ok && test_timeouts_and_cancel(shared_dir);
    if (ok) {
        ok = get_until_preserves_completion_when_abandon_loses();
    }
    if (ok) {
        append_client_event(shared_dir, "abandon_loses_race_returned_completion");
    }

    sintra::barrier(k_done_barrier, "_sintra_all_processes");
    return ok ? 0 : 1;
}

void print_event_mismatch(
    const std::vector<std::string>&   expected,
    const std::vector<std::string>&   actual)
{
    std::fprintf(stderr, "%.*sclient events mismatch\n",
        static_cast<int>(k_prefix.size()),
        k_prefix.data());
    std::fprintf(stderr, "expected:\n");
    for (const auto& entry : expected) {
        std::fprintf(stderr, "  %s\n", entry.c_str());
    }
    std::fprintf(stderr, "actual:\n");
    for (const auto& entry : actual) {
        std::fprintf(stderr, "  %s\n", entry.c_str());
    }
}

int verify_results(const std::filesystem::path& shared_dir)
{
    const std::vector<std::string> expected = {
        "same_process_non_strict_rejected",
        "same_process_strict_non_void_returned",
        "same_process_strict_void_returned",
        "remote_non_void_returned",
        "remote_void_returned",
        "remote_logic_identity_preserved",
        "remote_unavailable_identity_preserved",
        "timeout_non_void_abandoned",
        "timeout_void_abandoned",
        "cancelled_identity_preserved",
        "abandon_loses_race_returned_completion",
    };

    const auto actual = sintra::test::read_lines(client_events_path(shared_dir));
    if (actual != expected) {
        print_event_mismatch(expected, actual);
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "rpc_bounded_result",
        {process_owner,
         process_client},
        [](const std::filesystem::path&) {
            sintra::barrier(k_done_barrier, "_sintra_all_processes");
            return 0;
        },
        verify_results);
}
