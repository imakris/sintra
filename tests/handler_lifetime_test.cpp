#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view k_failure_prefix = "handler_lifetime_test: ";
constexpr std::string_view k_no_shutdown_child_arg =
    "--handler-lifetime-no-shutdown-child";
constexpr int              k_event_value    = 19;

struct Handler_lifetime_bus : sintra::Derived_transceiver<Handler_lifetime_bus>
{
    using Derived_transceiver::Derived_transceiver;

    SINTRA_MESSAGE(Event_message, int value);
};

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

class Thread_completion_marker
{
public:
    explicit Thread_completion_marker(std::atomic<bool>& completed) noexcept
    :
        m_completed(completed)
    {}

    ~Thread_completion_marker() noexcept
    {
        m_completed.store(true, std::memory_order_release);
    }

    Thread_completion_marker(const Thread_completion_marker&) = delete;
    Thread_completion_marker& operator=(const Thread_completion_marker&) = delete;

private:
    std::atomic<bool>& m_completed;
};

void join_completed_thread(
    std::thread&             thread,
    const std::atomic<bool>& completed,
    const char*              timeout_message)
{
    if (!thread.joinable()) {
        return;
    }

    const bool thread_completed = wait_until(
        [&]() { return completed.load(std::memory_order_acquire); },
        std::chrono::seconds(2));
    sintra::test::require_true(thread_completed, k_failure_prefix, timeout_message);
    thread.join();
}

void dispatch_direct_event(int value)
{
    Handler_lifetime_bus::Event_message message(value);
    message.sender_instance_id   = sintra::invalid_instance_id;
    message.receiver_instance_id = sintra::any_local_or_remote;

    sintra::dispatch_event_handlers(
        message,
        {sintra::any_local_or_remote});
}

struct Copied_slot_capture
{
    int  first_entries = 0;
    int  second_entries = 0;
    bool second_deactivated = false;
};

void run_deactivating_handler_skips_later_copied_slot()
{
    Copied_slot_capture capture;
    sintra::Transceiver::handler_deactivator first_slot;
    sintra::Transceiver::handler_deactivator second_slot;
    bool first_slot_active  = false;
    bool second_slot_active = false;

    try {
        first_slot = sintra::activate_slot(
            [&](const Handler_lifetime_bus::Event_message& message) {
                (void)message;
                ++capture.first_entries;
                second_slot();
                capture.second_deactivated = true;
                second_slot_active = false;
            });
        first_slot_active = true;

        second_slot = sintra::activate_slot(
            [&](const Handler_lifetime_bus::Event_message& message) {
                (void)message;
                ++capture.second_entries;
            });
        second_slot_active = true;

        dispatch_direct_event(k_event_value);

        first_slot();
        first_slot_active = false;

        sintra::test::require_true(capture.first_entries == 1, k_failure_prefix,
            "first copied-slot handler did not run exactly once");
        sintra::test::require_true(capture.second_deactivated, k_failure_prefix,
            "first handler did not deactivate the later copied slot");
        sintra::test::require_true(capture.second_entries == 0, k_failure_prefix,
            "later copied slot ran after its deactivator returned");
    }
    catch (...) {
        if (first_slot_active) {
            first_slot();
        }
        if (second_slot_active) {
            second_slot();
        }
        throw;
    }
}

struct External_deactivation_capture
{
    std::mutex              mutex;
    std::condition_variable condition;
    bool                    handler_entered = false;
    bool                    release_handler = false;
    bool                    deactivator_started = false;
    bool                    deactivator_returned = false;
    bool                    deactivator_exception = false;
};

void run_external_deactivation_waits_for_active_invocation()
{
    External_deactivation_capture capture;
    sintra::Transceiver::handler_deactivator slot;
    bool slot_active = false;
    std::thread dispatch_thread;
    std::thread deactivator_thread;
    std::exception_ptr dispatch_thread_exception;
    std::atomic<bool> dispatch_thread_done{false};
    std::atomic<bool> deactivator_thread_done{false};

    auto release_handler = [&]() {
        {
            std::lock_guard<std::mutex> lock(capture.mutex);
            capture.release_handler = true;
        }
        capture.condition.notify_all();
    };

    try {
        slot = sintra::activate_slot(
            [&](const Handler_lifetime_bus::Event_message& message) {
                (void)message;
                std::unique_lock<std::mutex> lock(capture.mutex);
                capture.handler_entered = true;
                capture.condition.notify_all();
                capture.condition.wait(lock, [&]() { return capture.release_handler; });
            });
        slot_active = true;

        dispatch_thread = std::thread([&]() {
            Thread_completion_marker completion(dispatch_thread_done);
            try {
                dispatch_direct_event(k_event_value);
            }
            catch (...) {
                dispatch_thread_exception = std::current_exception();
            }
        });

        bool handler_entered = false;
        {
            std::unique_lock<std::mutex> lock(capture.mutex);
            handler_entered = capture.condition.wait_for(
                lock,
                std::chrono::seconds(2),
                [&]() { return capture.handler_entered; });
        }
        sintra::test::require_true(handler_entered, k_failure_prefix,
            "timed out waiting for active handler invocation");

        deactivator_thread = std::thread([&]() {
            Thread_completion_marker completion(deactivator_thread_done);
            {
                std::lock_guard<std::mutex> lock(capture.mutex);
                capture.deactivator_started = true;
            }
            capture.condition.notify_all();

            bool failed = false;
            try {
                slot();
            }
            catch (...) {
                failed = true;
            }

            {
                std::lock_guard<std::mutex> lock(capture.mutex);
                capture.deactivator_exception = failed;
                capture.deactivator_returned  = true;
            }
            capture.condition.notify_all();
        });

        bool deactivator_started = false;
        bool returned_before_release = false;
        {
            std::unique_lock<std::mutex> lock(capture.mutex);
            deactivator_started = capture.condition.wait_for(
                lock,
                std::chrono::seconds(2),
                [&]() { return capture.deactivator_started; });
            if (deactivator_started) {
                returned_before_release = capture.condition.wait_for(
                    lock,
                    std::chrono::milliseconds(100),
                    [&]() { return capture.deactivator_returned; });
            }
        }
        sintra::test::require_true(deactivator_started, k_failure_prefix,
            "timed out waiting for external deactivator");
        sintra::test::require_true(!returned_before_release, k_failure_prefix,
            "external deactivator returned while its handler was still active");

        release_handler();
        join_completed_thread(dispatch_thread, dispatch_thread_done,
            "dispatch thread did not finish after active handler release");
        join_completed_thread(deactivator_thread, deactivator_thread_done,
            "external deactivator thread did not finish after active handler release");
        slot_active = false;

        if (dispatch_thread_exception) {
            std::rethrow_exception(dispatch_thread_exception);
        }
        sintra::test::require_true(!capture.deactivator_exception, k_failure_prefix,
            "external deactivator threw");
    }
    catch (...) {
        release_handler();
        join_completed_thread(dispatch_thread, dispatch_thread_done,
            "dispatch thread did not finish during cleanup");
        join_completed_thread(deactivator_thread, deactivator_thread_done,
            "deactivator thread did not finish during cleanup");
        if (slot_active) {
            slot();
        }
        throw;
    }
}

struct Self_deactivation_capture
{
    std::mutex              mutex;
    std::condition_variable condition;
    int                     entries = 0;
    bool                    first_handler_entered = false;
    bool                    second_dispatch_started = false;
    bool                    second_entered_before_deactivation = false;
    bool                    deactivation_returned = false;
    bool                    deactivation_exception = false;
    bool                    abort_wait = false;
};

void run_self_deactivation_suppresses_queued_same_slot_dispatch()
{
    Self_deactivation_capture capture;
    sintra::Transceiver::handler_deactivator slot;
    bool slot_active = false;
    std::thread first_thread;
    std::thread second_thread;
    std::exception_ptr first_thread_exception;
    std::exception_ptr second_thread_exception;
    std::atomic<bool> first_thread_done{false};
    std::atomic<bool> second_thread_done{false};

    try {
        slot = sintra::activate_slot(
            [&](const Handler_lifetime_bus::Event_message& message) {
                (void)message;
                std::unique_lock<std::mutex> lock(capture.mutex);
                ++capture.entries;
                if (capture.entries == 1) {
                    capture.first_handler_entered = true;
                    capture.condition.notify_all();
                    capture.condition.wait(lock, [&]() {
                        return capture.second_dispatch_started || capture.abort_wait;
                    });
                    if (capture.abort_wait) {
                        return;
                    }
                    capture.second_entered_before_deactivation = capture.condition.wait_for(
                        lock,
                        std::chrono::milliseconds(200),
                        [&]() { return capture.entries > 1; });

                    lock.unlock();
                    bool failed = false;
                    try {
                        slot();
                    }
                    catch (...) {
                        failed = true;
                    }
                    lock.lock();

                    capture.deactivation_exception = failed;
                    capture.deactivation_returned  = true;
                    capture.condition.notify_all();
                    return;
                }
                capture.condition.notify_all();
            });
        slot_active = true;

        first_thread = std::thread([&]() {
            Thread_completion_marker completion(first_thread_done);
            try {
                dispatch_direct_event(k_event_value);
            }
            catch (...) {
                first_thread_exception = std::current_exception();
            }
        });

        bool first_entered = false;
        {
            std::unique_lock<std::mutex> lock(capture.mutex);
            first_entered = capture.condition.wait_for(
                lock,
                std::chrono::seconds(2),
                [&]() { return capture.first_handler_entered; });
        }
        sintra::test::require_true(first_entered, k_failure_prefix,
            "timed out waiting for first self-deactivating handler");

        second_thread = std::thread([&]() {
            Thread_completion_marker completion(second_thread_done);
            {
                std::lock_guard<std::mutex> lock(capture.mutex);
                capture.second_dispatch_started = true;
            }
            capture.condition.notify_all();
            try {
                dispatch_direct_event(k_event_value);
            }
            catch (...) {
                second_thread_exception = std::current_exception();
            }
        });

        join_completed_thread(first_thread, first_thread_done,
            "first self-deactivation dispatch thread did not finish");
        join_completed_thread(second_thread, second_thread_done,
            "queued same-slot dispatch thread did not finish");
        slot_active = false;

        if (first_thread_exception) {
            std::rethrow_exception(first_thread_exception);
        }
        if (second_thread_exception) {
            std::rethrow_exception(second_thread_exception);
        }

        sintra::test::require_true(capture.deactivation_returned, k_failure_prefix,
            "self-deactivator did not return");
        sintra::test::require_true(!capture.deactivation_exception, k_failure_prefix,
            "self-deactivator threw");
        sintra::test::require_true(!capture.second_entered_before_deactivation, k_failure_prefix,
            "same-slot handler entered concurrently before self-deactivation");
        sintra::test::require_true(capture.entries == 1, k_failure_prefix,
            "queued same-slot dispatch ran after self-deactivation");
    }
    catch (...) {
        {
            std::lock_guard<std::mutex> lock(capture.mutex);
            capture.abort_wait = true;
        }
        capture.condition.notify_all();
        join_completed_thread(first_thread, first_thread_done,
            "first self-deactivation dispatch thread did not finish during cleanup");
        join_completed_thread(second_thread, second_thread_done,
            "queued same-slot dispatch thread did not finish during cleanup");
        if (slot_active && !capture.deactivation_returned) {
            slot();
        }
        throw;
    }
}

void run_deactivate_all_slots_invokes_stable_deactivator_copies()
{
    constexpr int k_slot_count = 256;

    std::vector<sintra::Transceiver::handler_deactivator> deactivators;
    deactivators.reserve(k_slot_count);
    std::atomic<int> entries{0};

    try {
        for (int i = 0; i < k_slot_count; ++i) {
            const std::string payload(64 + (i % 8), static_cast<char>('a' + (i % 26)));
            deactivators.emplace_back(sintra::activate_slot(
                [payload, &entries](const Handler_lifetime_bus::Event_message& message) {
                    if (!payload.empty() && message.value == k_event_value) {
                        entries.fetch_add(1, std::memory_order_acq_rel);
                    }
                }));
        }

        sintra::deactivate_all_slots();

        dispatch_direct_event(k_event_value);
        sintra::test::require_true(
            entries.load(std::memory_order_acquire) == 0,
            k_failure_prefix,
            "deactivate_all_slots left copied handlers dispatchable");

        for (auto& deactivator : deactivators) {
            if (deactivator) {
                deactivator();
            }
        }
    }
    catch (...) {
        for (auto& deactivator : deactivators) {
            if (deactivator) {
                try {
                    deactivator();
                }
                catch (...) {
                }
            }
        }
        throw;
    }
}

void arm_static_cleanup_guard_with_live_slots()
{
    constexpr int k_slot_count = 32;

    for (int i = 0; i < k_slot_count; ++i) {
        const std::string payload(32 + (i % 4), static_cast<char>('A' + (i % 26)));
        (void)sintra::activate_slot(
            [payload](const Handler_lifetime_bus::Event_message& message) {
                (void)message;
                (void)payload;
            });
    }
}

std::string quote_command_arg(std::string_view value)
{
    return std::string("\"") + std::string(value) + "\"";
}

void run_no_shutdown_cleanup_guard_child_process(char* binary_path)
{
    const std::string command =
        quote_command_arg(binary_path) + " " + std::string(k_no_shutdown_child_arg);
    const int child_status = std::system(command.c_str());
    sintra::test::require_true(child_status == 0, k_failure_prefix,
        "no-explicit-shutdown cleanup-guard child failed");
}

} // namespace

int main(int argc, char* argv[])
{
    if (sintra::test::has_argv_flag(argc, argv, k_no_shutdown_child_arg)) {
        try {
            const char* child_argv[] = {argv[0]};
            sintra::init(1, child_argv);
            arm_static_cleanup_guard_with_live_slots();
            return 0;
        }
        catch (const std::exception& ex) {
            std::cerr << "handler_lifetime_test child failed: " << ex.what() << std::endl;
            return 1;
        }
    }

    bool initialized = false;

    try {
        sintra::init(argc, const_cast<const char* const*>(argv));
        initialized = true;

        run_deactivating_handler_skips_later_copied_slot();
        run_external_deactivation_waits_for_active_invocation();
        run_self_deactivation_suppresses_queued_same_slot_dispatch();
        run_deactivate_all_slots_invokes_stable_deactivator_copies();

        sintra::shutdown();
        initialized = false;

        run_no_shutdown_cleanup_guard_child_process(argv[0]);
    }
    catch (const std::exception& ex) {
        if (initialized) {
            try {
                sintra::shutdown();
            }
            catch (...) {
            }
        }
        std::cerr << "handler_lifetime_test failed: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
