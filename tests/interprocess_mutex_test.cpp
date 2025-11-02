#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <system_error>
#include <thread>

using namespace std::chrono_literals;

#include "sintra/detail/ipc/mutex.h"
#include "sintra/detail/ipc/platform_utils.h"

namespace
{

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "interprocess_mutex_test failure: " << message << std::endl;
    std::exit(1);
}

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void expect_error_code(const std::system_error& error, std::errc expected, const std::string& context)
{
    if (error.code() != std::make_error_code(expected)) {
        std::cerr << context << " threw unexpected error: " << error.code().message() << std::endl;
        std::exit(1);
    }
}

class test_mutex : public sintra::detail::interprocess_mutex
{
public:
    using owner_token = std::uint64_t;

    void set_raw_owner(owner_token token)
    {
        owner_atomic().store(token, std::memory_order_release);
    }

    owner_token raw_owner() const
    {
        return owner_atomic().load(std::memory_order_acquire);
    }

    void set_recovering(uint32_t pid)
    {
        recovering_atomic().store(pid, std::memory_order_release);
    }

    static owner_token make_current_owner_token()
    {
        const owner_token pid = static_cast<owner_token>(sintra::get_current_pid());
        const owner_token tid = static_cast<owner_token>(sintra::get_current_tid());
        return (pid << 32u) | (tid & 0xFFFFFFFFull);
    }

private:
    std::atomic<owner_token>& owner_atomic()
    {
        return *reinterpret_cast<std::atomic<owner_token>*>(this);
    }

    const std::atomic<owner_token>& owner_atomic() const
    {
        return *reinterpret_cast<const std::atomic<owner_token>*>(this);
    }

    std::atomic<uint32_t>& recovering_atomic()
    {
        auto* ptr = reinterpret_cast<std::atomic<uint32_t>*>(
            reinterpret_cast<char*>(this) + sizeof(std::atomic<owner_token>));
        return *ptr;
    }

    const std::atomic<uint32_t>& recovering_atomic() const
    {
        auto* ptr = reinterpret_cast<const std::atomic<uint32_t>*>(
            reinterpret_cast<const char*>(this) + sizeof(std::atomic<owner_token>));
        return *ptr;
    }
};

} // namespace

int main()
{
    test_mutex mtx;

    // Fresh mutex should be acquirable via try_lock.
    expect(mtx.try_lock(), "try_lock should succeed on an unlocked mutex");

    // Recursive try_lock must throw resource_deadlock_would_occur.
    bool try_lock_detected_recursion = false;
    try {
        (void)mtx.try_lock();
    } catch (const std::system_error& error) {
        expect_error_code(error, std::errc::resource_deadlock_would_occur,
                          "try_lock recursion");
        try_lock_detected_recursion = true;
    }
    expect(try_lock_detected_recursion, "try_lock recursion should throw");

    // While locked, another thread attempting try_lock should fail without throwing.
    std::atomic<bool> other_thread_attempted{false};
    std::atomic<bool> other_thread_acquired{false};
    std::atomic<bool> other_thread_threw{false};

    std::thread contender([&] {
        other_thread_attempted.store(true, std::memory_order_release);
        try {
            bool acquired = mtx.try_lock();
            other_thread_acquired.store(acquired, std::memory_order_release);
            if (acquired) {
                mtx.unlock();
            }
        } catch (...) {
            other_thread_threw.store(true, std::memory_order_release);
        }
    });

    while (!other_thread_attempted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    contender.join();

    expect(!other_thread_threw.load(std::memory_order_acquire),
           "try_lock from another thread should not throw");
    expect(!other_thread_acquired.load(std::memory_order_acquire),
           "try_lock from another thread should fail while mutex is locked");

    // Unlock for subsequent tests.
    mtx.unlock();

    // A new thread should be able to lock/unlock successfully now.
    std::atomic<bool> second_thread_locked{false};
    std::thread locker([&] {
        mtx.lock();
        second_thread_locked.store(true, std::memory_order_release);
        std::this_thread::sleep_for(1ms);
        mtx.unlock();
    });

    for (int i = 0; i < 100 && !second_thread_locked.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    locker.join();
    expect(second_thread_locked.load(std::memory_order_acquire),
           "second thread should acquire the mutex after it is unlocked");

    // Unlock attempt by a non-owner thread must throw.
    mtx.lock();
    std::atomic<bool> non_owner_detected{false};
    std::thread non_owner([&] {
        try {
            mtx.unlock();
        } catch (const std::system_error& error) {
            expect_error_code(error, std::errc::operation_not_permitted,
                              "unlock from non-owner");
            non_owner_detected.store(true, std::memory_order_release);
        } catch (...) {
        }
    });
    non_owner.join();
    expect(non_owner_detected.load(std::memory_order_acquire),
           "unlock from non-owner should throw operation_not_permitted");
    mtx.unlock();

    // Recursive lock must throw resource_deadlock_would_occur.
    bool lock_detected_recursion = false;
    mtx.lock();
    try {
        mtx.lock();
    } catch (const std::system_error& error) {
        expect_error_code(error, std::errc::resource_deadlock_would_occur,
                          "lock recursion");
        lock_detected_recursion = true;
    }
    expect(lock_detected_recursion, "lock recursion should throw");
    mtx.unlock();

    // Recovery from a dead owner should succeed.
    test_mutex recovery;
    const auto dead_pid = static_cast<uint32_t>(0);
    // PID 0 is reserved on all supported platforms and treated as always-dead by
    // is_process_alive, guaranteeing deterministic recovery behaviour.
    const test_mutex::owner_token dead_owner =
        (static_cast<test_mutex::owner_token>(dead_pid) << 32u) | 0x12345678ull;

    recovery.set_raw_owner(dead_owner);
    recovery.set_recovering(0);

    recovery.lock();
    const auto expected_owner = test_mutex::make_current_owner_token();
    expect(recovery.raw_owner() == expected_owner,
           "lock should recover ownership from a dead process");

    recovery.unlock();
    expect(recovery.try_lock(), "mutex should be usable after recovery");
    recovery.unlock();

    return 0;
}
