#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <system_error>
#include <thread>

using namespace std::chrono_literals;

#include "test_utils.h"

#include "sintra/detail/ipc/mutex.h"

namespace
{

constexpr std::string_view k_failure_prefix = "interprocess_mutex_test failure: ";

void expect_error_code(
    const std::system_error&   error,
    std::errc                  expected,
    const std::string&         context)
{
    if (error.code() != std::make_error_code(expected)) {
        std::cerr << context << " threw unexpected error: " << error.code().message() << std::endl;
        std::exit(1);
    }
}

using Test_mutex = sintra::detail::interprocess_mutex;
using owner_token = std::uint64_t;

owner_token make_owner_token(uint32_t pid, uint32_t tid)
{
    return (static_cast<owner_token>(pid) << 32u) |
        (static_cast<owner_token>(tid) & 0xFFFFFFFFull);
}

owner_token make_current_owner_token()
{
    return make_owner_token(
        static_cast<uint32_t>(sintra::get_current_pid()),
        static_cast<uint32_t>(sintra::get_current_tid()));
}

void install_owner_fixture(
    Test_mutex&   mutex,
    uint32_t      pid,
    uint32_t      tid,
    uint64_t      start_stamp)
{
    mutex.test_install_owner_fixture({pid, tid, start_stamp});
}

void run_owner_generation_recovery_red_gate()
{
    const auto current_start_stamp = sintra::current_process_start_stamp();
    if (!current_start_stamp || *current_start_stamp == 0) {
        sintra::test::expect(false, k_failure_prefix,
            "current_process_start_stamp is required for owner-generation recovery red gate");
    }

    const auto current_pid = static_cast<uint32_t>(sintra::get_current_pid());
    const auto current_tid = static_cast<uint32_t>(sintra::get_current_tid());
    const auto other_tid = (current_tid == 1u) ? 2u : 1u;
    const auto stale_start_stamp =
        (*current_start_stamp == 1u) ? 2u : (*current_start_stamp - 1u);

    bool ok = true;
    auto require_recovery = [&](uint32_t tid, std::string_view context) {
        Test_mutex mutex;
        install_owner_fixture(mutex, current_pid, tid, stale_start_stamp);
        const bool acquired = mutex.try_lock_for(20ms);
        ok &= sintra::test::assert_true(acquired, k_failure_prefix, context);
        if (acquired) {
            mutex.unlock();
        }
    };

    auto require_no_recovery = [&](uint32_t tid, std::string_view context) {
        Test_mutex mutex;
        install_owner_fixture(mutex, current_pid, tid, *current_start_stamp);
        const auto seeded_owner = mutex.test_owner_token();
        const bool acquired = mutex.try_lock_for(20ms);
        const auto after_owner = mutex.test_owner_token();
        ok &= sintra::test::assert_true(!acquired, k_failure_prefix, context);
        ok &= sintra::test::assert_true(after_owner == seeded_owner,
            k_failure_prefix,
            "live current-generation owner token should remain unchanged");
        if (acquired) {
            mutex.unlock();
        }
    };

    require_recovery(other_tid,
        "stale-generation owner with current pid and different tid should recover");
    require_recovery(current_tid,
        "stale-generation owner with current pid and current tid should recover");
    require_no_recovery(other_tid,
        "live current-generation owner with different tid should not recover");
    require_no_recovery(current_tid,
        "live current-generation owner with current tid should not recover");

    sintra::test::expect(ok, k_failure_prefix,
        "owner-generation recovery red gate failed");
}

} // namespace

int main()
{
    Test_mutex mtx;

    // Fresh mutex should be acquirable via try_lock.
    sintra::test::expect(mtx.try_lock(), k_failure_prefix,
        "try_lock should succeed on an unlocked mutex");

    // Recursive try_lock must return false (same thread cannot reacquire).
    bool recursive_try_lock_result = mtx.try_lock();
    sintra::test::expect(!recursive_try_lock_result, k_failure_prefix,
        "try_lock recursion should return false");

    // While locked, another thread attempting try_lock should fail without throwing.
    std::atomic<bool> other_thread_attempted{false};
    std::atomic<bool> other_thread_acquired{false};
    std::atomic<bool> other_thread_threw{false};

    std::thread contender([&] {
        other_thread_attempted.store(true);
        try {
            bool acquired = mtx.try_lock();
            other_thread_acquired.store(acquired);
            if (acquired) {
                mtx.unlock();
            }
        }
        catch (...) {
            other_thread_threw.store(true);
        }
    });

    while (!other_thread_attempted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    contender.join();

    sintra::test::expect(!other_thread_threw.load(std::memory_order_acquire),
        k_failure_prefix,
        "try_lock from another thread should not throw");
    sintra::test::expect(!other_thread_acquired.load(std::memory_order_acquire),
        k_failure_prefix,
        "try_lock from another thread should fail while mutex is locked");

    // Unlock for subsequent tests.
    mtx.unlock();

    // A new thread should be able to lock/unlock successfully now.
    std::atomic<bool> second_thread_locked{false};
    std::thread locker([&] {
        mtx.lock();
        second_thread_locked.store(true);
        std::this_thread::sleep_for(1ms);
        mtx.unlock();
    });

    for (int i = 0; i < 100 && !second_thread_locked.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    locker.join();
    sintra::test::expect(second_thread_locked.load(std::memory_order_acquire),
        k_failure_prefix,
        "second thread should acquire the mutex after it is unlocked");

    // Unlock attempt by a non-owner thread must throw.
    mtx.lock();
    std::atomic<bool> non_owner_detected{false};
    std::thread non_owner([&] {
        try {
            mtx.unlock();
        }
        catch (const std::system_error& error) {
            expect_error_code(error, std::errc::operation_not_permitted,
                "unlock from non-owner");
            non_owner_detected.store(true);
        }
        catch (...) {
        }
    });
    non_owner.join();
    sintra::test::expect(non_owner_detected.load(std::memory_order_acquire),
        k_failure_prefix,
        "unlock from non-owner should throw operation_not_permitted");
    mtx.unlock();

    // Recursive lock must throw resource_deadlock_would_occur.
    bool lock_detected_recursion = false;
    mtx.lock();
    try {
        mtx.lock();
    }
    catch (const std::system_error& error) {
        expect_error_code(error, std::errc::resource_deadlock_would_occur,
            "lock recursion");
        lock_detected_recursion = true;
    }
    sintra::test::expect(lock_detected_recursion, k_failure_prefix,
        "lock recursion should throw");
    mtx.unlock();

    // Test try_lock_for with immediate success.
    {
        Test_mutex timed_mtx;
        bool acquired = timed_mtx.try_lock_for(100ms);
        sintra::test::expect(acquired, k_failure_prefix,
            "try_lock_for should succeed on unlocked mutex");
        timed_mtx.unlock();
    }

    // Test try_lock_for with timeout (mutex held by another thread).
    {
        Test_mutex timed_mtx;
        std::atomic<bool> holding{false};
        std::atomic<bool> release{false};

        std::thread holder([&] {
            timed_mtx.lock();
            holding.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            timed_mtx.unlock();
        });

        while (!holding.load()) {
            std::this_thread::sleep_for(1ms);
        }

        bool acquired = timed_mtx.try_lock_for(50ms);
        if (acquired) {
            timed_mtx.unlock();
        }
        sintra::test::expect(!acquired, k_failure_prefix,
            "try_lock_for should timeout when mutex is held");

        release.store(true);
        holder.join();
    }

    // Test try_lock_until with immediate success.
    {
        Test_mutex timed_mtx;
        auto deadline = std::chrono::steady_clock::now() + 100ms;
        bool acquired = timed_mtx.try_lock_until(deadline);
        sintra::test::expect(acquired, k_failure_prefix,
            "try_lock_until should succeed on unlocked mutex");
        timed_mtx.unlock();
    }

    // Test try_lock_until with past deadline (mutex held by another thread).
    {
        Test_mutex timed_mtx;
        std::atomic<bool> holding{false};
        std::atomic<bool> release{false};

        std::thread holder([&] {
            timed_mtx.lock();
            holding.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            timed_mtx.unlock();
        });

        while (!holding.load()) {
            std::this_thread::sleep_for(1ms);
        }

        auto past_deadline = std::chrono::steady_clock::now() - 10ms;
        bool acquired      = timed_mtx.try_lock_until(past_deadline);
        if (acquired) {
            timed_mtx.unlock();
        }
        sintra::test::expect(!acquired, k_failure_prefix,
            "try_lock_until with past deadline should fail");

        release.store(true);
        holder.join();
    }

    // Recovery from a dead owner should succeed.
    Test_mutex recovery;
    const auto dead_pid = static_cast<uint32_t>(0);
    // PID 0 is reserved on all supported platforms and treated as always-dead by
    // is_process_alive, guaranteeing deterministic recovery behaviour.
    install_owner_fixture(recovery, dead_pid, 0x12345678u, 0);

    recovery.lock();
    const auto expected_owner = make_current_owner_token();
    sintra::test::expect(recovery.test_owner_token() == expected_owner,
        k_failure_prefix,
        "lock should recover ownership from a dead process");

    recovery.unlock();
    sintra::test::expect(recovery.try_lock(), k_failure_prefix,
        "mutex should be usable after recovery");
    recovery.unlock();

    run_owner_generation_recovery_red_gate();

    return 0;
}
