#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_prefix        = "rpc_stop_result_test: ";
constexpr const char*      k_service_name  = "stop result service";
constexpr const char*      k_ready_barrier = "stop-result-ready";
constexpr const char*      k_done_barrier  = "stop-result-done";
constexpr const char*      k_logic_error   = "approval operation rejected";
constexpr int              k_reply         = 317;

bool check(bool condition, std::string_view message)
{
    return sintra::test::assert_true(condition, k_prefix, message);
}

std::filesystem::path marker_path(
    const std::filesystem::path& directory,
    const std::string& tag,
    const char* stage)
{
    return directory / (tag + "_" + stage);
}

void write_marker(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create marker: " + path.string());
    }
    out << "ready\n";
}

void held_work(const std::string& directory, const std::string& tag)
{
    write_marker(marker_path(directory, tag, "started"));
    if (!sintra::test::wait_for_file(marker_path(directory, tag, "released"), 10s)) {
        throw std::runtime_error("held RPC was not released");
    }
    write_marker(marker_path(directory, tag, "finished"));
}

struct Stop_service : sintra::Derived_transceiver<Stop_service>
{
    int echo(int value) { return value; }
    void ready_void() {}

    int delayed_reply(int value)
    {
        // A slow handler must not become a caller-side timeout.
        std::this_thread::sleep_for(6s);
        return value;
    }

    int held_reply(std::string directory, std::string tag, int value)
    {
        held_work(directory, tag);
        return value;
    }

    void held_void(std::string directory, std::string tag)
    {
        held_work(directory, tag);
    }

    int throwing_reply()
    {
        throw std::logic_error(k_logic_error);
    }

    SINTRA_RPC_STRICT(echo)
    SINTRA_RPC_STRICT(ready_void)
    SINTRA_RPC_STRICT(delayed_reply)
    SINTRA_RPC_STRICT(held_reply)
    SINTRA_RPC_STRICT(held_void)
    SINTRA_RPC_STRICT(throwing_reply)
};

template <typename Handle>
bool stopped_call_preserves_remote_work(
    Handle& handle,
    const std::filesystem::path& directory,
    const std::string& tag,
    bool already_stopped)
{
    if (!check(sintra::test::wait_for_file(marker_path(directory, tag, "started"), 5s),
            "remote handler must accept the original call before stop"))
    {
        write_marker(marker_path(directory, tag, "released"));
        return false;
    }

    std::stop_source stop;
    bool cancelled = false;
    {
        std::jthread requester;
        if (already_stopped) {
            stop.request_stop();
        }
        else {
            requester = std::jthread([&stop] {
                std::this_thread::sleep_for(50ms);
                stop.request_stop();
            });
        }

        try {
            (void)handle.get(stop.get_token());
        }
        catch (const sintra::rpc_cancelled&) {
            cancelled = true;
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "%.*sunexpected stop outcome: %s\n",
                (int)k_prefix.size(), k_prefix.data(), error.what());
        }
    }

    bool ok = check(cancelled, "a requested stop must end the pending caller wait");
    ok &= check(!std::filesystem::exists(marker_path(directory, tag, "finished")),
        "caller stop must finish while the original remote work remains held");

    write_marker(marker_path(directory, tag, "released"));
    ok &= check(sintra::test::wait_for_file(marker_path(directory, tag, "finished"), 5s),
        "original remote work must continue after caller abandonment");
    ok &= check(Stop_service::rpc_echo(k_service_name, k_reply) == k_reply,
        "the remote service must remain usable after its late reply");

    bool abandoned = false;
    try {
        (void)handle.get();
    }
    catch (const sintra::rpc_cancelled&) {
    }
    catch (const std::runtime_error&) {
        abandoned = true;
    }
    ok &= check(abandoned, "a stopped handle must retain its caller-abandoned outcome");
    ok &= check(sintra::s_mproc->unblock_rpc() == 0,
        "stopped calls must release their outstanding RPC registrations");
    return ok;
}

bool completed_calls_win_stop()
{
    std::stop_source stop;

    auto value = Stop_service::rpc_async_echo(k_service_name, k_reply);
    bool ok = check(value.get() == k_reply, "the value reply must arrive before stop");

    auto completed_void = Stop_service::rpc_async_ready_void(k_service_name);
    completed_void.get();

    auto error = Stop_service::rpc_async_throwing_reply(k_service_name);
    bool remote_error = false;
    try {
        (void)error.get();
    }
    catch (const std::logic_error& failure) {
        remote_error = std::string_view(failure.what()) == k_logic_error;
    }
    ok &= check(remote_error, "the remote error must arrive before stop");

    stop.request_stop();
    ok &= check(value.get(stop.get_token()) == k_reply,
        "an already-completed value must win an already-requested stop");
    completed_void.get(stop.get_token());

    remote_error = false;
    try {
        (void)error.get(stop.get_token());
    }
    catch (const std::logic_error& failure) {
        remote_error = std::string_view(failure.what()) == k_logic_error;
    }
    ok &= check(remote_error, "an already-completed remote error must preserve its type and message");
    return ok;
}

bool removed_target_preserves_failure()
{
    auto target = std::make_unique<Stop_service>();
    const auto identity = target->instance_id();
    target.reset();

    auto handle = Stop_service::rpc_async_echo(identity, k_reply);
    try {
        (void)handle.get(std::stop_token{});
    }
    catch (const sintra::rpc_unavailable&) {
        return true;
    }
    return check(false, "a removed transported target must still return rpc_unavailable");
}

int process_owner()
{
    Stop_service service;
    service.assign_name(k_service_name);
    sintra::barrier(k_ready_barrier);
    sintra::barrier(k_done_barrier, "_sintra_all_processes");
    return 0;
}

int process_client()
{
    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "rpc_stop_result");
    const auto& directory = shared.path();
    sintra::barrier(k_ready_barrier);

    bool ok = true;
    try {
        auto delayed = Stop_service::rpc_async_delayed_reply(k_service_name, k_reply);
        ok &= check(delayed.get(std::stop_token{}) == k_reply,
            "a healthy delayed RPC must complete without a result deadline");
        ok &= completed_calls_win_stop();

        auto stopped_value = Stop_service::rpc_async_held_reply(
            k_service_name, directory.string(), "stopped_value", k_reply);
        ok &= stopped_call_preserves_remote_work(stopped_value, directory, "stopped_value", false);

        auto stopped_void = Stop_service::rpc_async_held_void(
            k_service_name, directory.string(), "stopped_void");
        ok &= stopped_call_preserves_remote_work(stopped_void, directory, "stopped_void", true);
        ok &= removed_target_preserves_failure();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%.*sunexpected exception: %s\n",
            (int)k_prefix.size(), k_prefix.data(), error.what());
        ok = false;
    }

    sintra::test::write_lines(directory / "result.txt", {ok ? "passed" : "failed"});
    sintra::barrier(k_done_barrier, "_sintra_all_processes");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    return sintra::test::run_multi_process_shutdown_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "rpc_stop_result",
        {process_owner, process_client},
        [](const std::filesystem::path&) {
            sintra::barrier(k_done_barrier, "_sintra_all_processes");
            return 0;
        },
        [](const std::filesystem::path& directory) {
            return sintra::test::read_lines(directory / "result.txt") ==
                std::vector<std::string>{"passed"} ? 0 : 1;
        });
}

#else

int main()
{
    std::fprintf(stderr, "rpc_stop_result_test: cooperative stop tokens unavailable; API not enabled\n");
    return 0;
}

#endif
