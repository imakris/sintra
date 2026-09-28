//
// Sintra RPC log-callback stall test
//
// The log callback is user code and may block. While it runs, Sintra must not
// hold a lock that other threads spin on: a shared spinlock hold that lasts
// longer than the 2 s live-owner timeout aborts the process.
//
// Rejection: the coordinator process hosts Stall_service, and two children call
// into it, so each child's request ring has its own reader thread there. One
// child calls a function that nothing exports; its reader rejects the request
// and logs a warning, which the coordinator's log callback blocks. Meanwhile the
// other child keeps calling an exported function, which its reader dispatches
// through the same RPC handler map. Those calls must be served while the
// callback blocks, and the rejection must still reach the caller.
//
// Shutdown: destroying a transceiver while one of its handlers runs logs a
// warning every 5 s, which a child's log callback blocks while two threads call
// another exported function of the same object, through the same instance map.
// They must be told that the target is shutting down while the callback blocks.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view k_failure_prefix     = "rpc_log_callback_stall_test: ";
constexpr const char*      k_service_name       = "rpc_log_callback_stall_service";
constexpr std::string_view k_rejection_warning  = "Received RPC for unknown message type";
constexpr std::string_view k_shutdown_warning   = "Transceiver shutdown is waiting for";

// The block outlasts the spinlock's 2 s live-owner timeout, and lasts until a
// concurrent call was served during it, up to the limit.
constexpr auto k_callback_block       = std::chrono::seconds(3);
constexpr auto k_callback_block_limit = std::chrono::seconds(15);
constexpr auto k_call_limit           = std::chrono::seconds(25);
constexpr auto k_call_interval        = std::chrono::milliseconds(1);

enum class Block_phase : int
{
    BEFORE,
    BLOCKING,
    RELEASED,
};

struct Log_callback_block
{
    std::string_view          trigger;
    std::atomic<Block_phase>  phase{Block_phase::BEFORE};
    std::atomic<int>          served_while_blocked{0};
};

Log_callback_block& log_callback_block()
{
    static Log_callback_block block;
    return block;
}

void note_served_call()
{
    auto& block = log_callback_block();
    if (block.phase.load() == Block_phase::BLOCKING) {
        ++block.served_while_blocked;
    }
}

// Blocks the first message that contains the trigger.
void block_first_trigger(sintra::log_level level, const char* message, void* user_data)
{
    sintra::detail::default_log_callback(level, message, nullptr);

    auto& block = *static_cast<Log_callback_block*>(user_data);
    if (!message || std::string_view(message).find(block.trigger) == std::string_view::npos) {
        return;
    }
    auto expected = Block_phase::BEFORE;
    if (!block.phase.compare_exchange_strong(expected, Block_phase::BLOCKING)) {
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= k_callback_block_limit) {
            break;
        }
        if (elapsed >= k_callback_block && block.served_while_blocked.load() != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    block.phase = Block_phase::RELEASED;
}

void install_blocking_log_callback(std::string_view trigger)
{
    auto& block = log_callback_block();
    block.trigger = trigger;
    sintra::set_log_callback(&block_first_trigger, &block);
}

struct Stall_service : sintra::Derived_transceiver<Stall_service>
{
    int ping()
    {
        note_served_call();
        return static_cast<int>(log_callback_block().phase.load());
    }
    SINTRA_RPC(ping)
};

// Never instantiated, so no process has a handler for its function.
struct Unexported_service : sintra::Derived_transceiver<Unexported_service>
{
    int missing() { return 0; }
    SINTRA_RPC(missing)
};

struct Shutdown_target : sintra::Derived_transceiver<Shutdown_target>
{
    // Keeps the execution guard that shutdown waits for until the blocked
    // warning has been released.
    int hold()
    {
        s_hold_entered = true;
        const auto deadline = std::chrono::steady_clock::now() + k_call_limit;
        while (log_callback_block().phase.load() != Block_phase::RELEASED) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return 0;
            }
            std::this_thread::sleep_for(k_call_interval);
        }
        return 1;
    }
    SINTRA_RPC(hold)

    int probe() { return 1; }
    SINTRA_RPC(probe)

    static inline std::atomic<bool> s_hold_entered{false};
};

void write_outcome(const char* file_name, const std::vector<std::string>& lines)
{
    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "rpc_log_callback_stall");
    sintra::test::write_lines(shared.path() / file_name, lines);
}

int run_rejected_caller()
{
    sintra::barrier("service-ready", "_sintra_all_processes");

    std::string outcome;
    try {
        (void)Unexported_service::rpc_missing(k_service_name);
        outcome = "unexpected-success";
    }
    catch (const sintra::rpc_unavailable& e) {
        outcome = std::string("rpc_unavailable:") + e.what();
    }
    catch (const std::exception& e) {
        outcome = std::string("other:") + e.what();
    }
    write_outcome("rejection.txt", {outcome});

    sintra::barrier("calls-finished", "_sintra_all_processes");
    return 0;
}

int run_pinger()
{
    sintra::barrier("service-ready", "_sintra_all_processes");

    bool released = false;
    std::string failure;
    const auto deadline = std::chrono::steady_clock::now() + k_call_limit;
    try {
        while (!released && std::chrono::steady_clock::now() < deadline) {
            const auto phase = static_cast<Block_phase>(Stall_service::rpc_ping(k_service_name));
            released = phase == Block_phase::RELEASED;
            std::this_thread::sleep_for(k_call_interval);
        }
    }
    catch (const std::exception& e) {
        failure = e.what();
    }
    write_outcome("pings.txt", {
        released ? "released" : "not-released",
        failure.empty() ? "no-failure" : "failure:" + failure});

    sintra::barrier("calls-finished", "_sintra_all_processes");
    return 0;
}

int run_shutdown_warning_stall()
{
    sintra::barrier("service-ready", "_sintra_all_processes");
    install_blocking_log_callback(k_shutdown_warning);

    auto target = std::make_unique<Shutdown_target>();
    const auto target_id = target->instance_id();

    int hold_result = -1;
    std::thread holder([&] {
        try {
            hold_result = Shutdown_target::rpc_hold(target_id);
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "%.*shold failed: %s\n",
                static_cast<int>(k_failure_prefix.size()), k_failure_prefix.data(), e.what());
        }
    });
    const auto entry_deadline = std::chrono::steady_clock::now() + k_call_limit;
    while (!Shutdown_target::s_hold_entered.load() &&
        std::chrono::steady_clock::now() < entry_deadline)
    {
        std::this_thread::sleep_for(k_call_interval);
    }

    std::atomic<bool> stop_probes{false};
    std::atomic<int>  unexpected_probe_outcomes{0};
    auto probe = [&] {
        while (!stop_probes.load()) {
            try {
                (void)Shutdown_target::rpc_probe(target_id);
            }
            catch (const sintra::rpc_unavailable&) {
                note_served_call();
            }
            catch (const std::exception&) {
                ++unexpected_probe_outcomes;
            }
            std::this_thread::sleep_for(k_call_interval);
        }
    };
    std::thread first_prober(probe);
    std::thread second_prober(probe);

    target->destroy();

    stop_probes = true;
    first_prober.join();
    second_prober.join();
    holder.join();
    target.reset();
    sintra::set_log_callback(nullptr);

    const auto& block = log_callback_block();
    write_outcome("shutdown.txt", {
        block.phase.load() == Block_phase::RELEASED ? "warning-released" : "warning-not-released",
        block.served_while_blocked.load() != 0 ? "refused-while-blocked" : "not-refused-while-blocked",
        hold_result == 1 ? "hold-returned" : "hold-failed",
        unexpected_probe_outcomes.load() == 0 ? "no-unexpected-probe-outcome" : "unexpected-probe-outcome"});

    sintra::barrier("calls-finished", "_sintra_all_processes");
    return 0;
}

bool expect_lines(
    const std::filesystem::path&     path,
    const std::vector<std::string>&  expected)
{
    const auto observed = sintra::test::read_lines(path);
    if (observed == expected) {
        return true;
    }
    std::string message = path.filename().string() + " mismatch; observed:";
    for (const auto& line : observed) {
        message += " [" + line + "]";
    }
    return sintra::test::assert_true(false, k_failure_prefix, message);
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "rpc_log_callback_stall",
        {run_rejected_caller, run_pinger, run_shutdown_warning_stall},
        [](const std::filesystem::path&) {
            install_blocking_log_callback(k_rejection_warning);
            bool published = false;
            {
                Stall_service service;
                published = service.assign_name(k_service_name);
                sintra::barrier("service-ready", "_sintra_all_processes");
                sintra::barrier("calls-finished", "_sintra_all_processes");
            }
            sintra::set_log_callback(nullptr);

            const auto& block = log_callback_block();
            bool passed = sintra::test::assert_true(published, k_failure_prefix,
                "could not publish the stall service");
            passed &= sintra::test::assert_true(
                block.phase.load() == Block_phase::RELEASED, k_failure_prefix,
                "the rejection warning never reached the log callback");
            passed &= sintra::test::assert_true(
                block.served_while_blocked.load() != 0, k_failure_prefix,
                "no targeted RPC was served while the rejection warning was blocked");
            return passed ? 0 : 1;
        },
        [](const std::filesystem::path& shared_dir) {
            bool passed = expect_lines(shared_dir / "rejection.txt",
                {"rpc_unavailable:RPC function is not available."});
            passed &= expect_lines(shared_dir / "pings.txt", {"released", "no-failure"});
            passed &= expect_lines(shared_dir / "shutdown.txt", {
                "warning-released",
                "refused-while-blocked",
                "hold-returned",
                "no-unexpected-probe-outcome"});
            return passed ? 0 : 1;
        });
}
