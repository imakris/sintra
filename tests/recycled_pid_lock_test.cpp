// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// Shared locks record their owner as a process instance: a PID and a token
// that each process image draws for itself. A lock recorded with this
// process's PID and another token was left by an earlier process with this
// PID, which has exited. A replacement process must recover such a lock in a
// ring's control block or lifecycle anchor without hanging or aborting, while
// a lock that this process holds, through any mapping, keeps excluding.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define private public
#define protected public
#include <sintra/detail/ipc/rings.h>
#undef protected
#undef private

#include "test_ring_utils.h"
#include "test_utils.h"

namespace {

using namespace std::chrono_literals;
using Writer = sintra::Ring_W<uint32_t>;
using Reader = sintra::Ring_R<uint32_t>;

constexpr std::string_view k_failure_prefix = "recycled_pid_lock_test: ";

void require(bool condition, std::string_view message)
{
    sintra::test::require_true(condition, k_failure_prefix, message);
}

uint64_t self_instance()
{
    return sintra::detail::current_process_instance();
}

// The token that an earlier process with this PID drew.
uint32_t earlier_token()
{
    return static_cast<uint32_t>(self_instance()) + 1u;
}

uint64_t earlier_instance()
{
    return (static_cast<uint64_t>(sintra::get_current_pid()) << 32) | earlier_token();
}

void install_spinlock_owner(sintra::spinlock& lock, uint64_t owner)
{
    lock.test_install_owner(owner, 1);
}

void install_earlier_mutex_owner(sintra::detail::interprocess_mutex& mutex)
{
    mutex.test_install_owner_fixture({
        sintra::get_current_pid(),
        sintra::get_current_tid(),
        sintra::current_process_start_stamp().value_or(0),
        earlier_token()});
}

// A reader died holding the slot stack's spinlock, and a replacement reader
// with its PID acquires a slot.
void ring_spinlock_left_by_earlier_process()
{
    sintra::test::Temp_ring_dir directory("recycled_pid_spinlock");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Reader keeper(directory.str(), "raw", elements);
    auto& lock = keeper.c.rs_stack_spinlock;

    install_spinlock_owner(lock, earlier_instance());
    lock.lock();
    const uint64_t owner = lock.test_owner();
    lock.unlock();
    require(owner == self_instance(),
        "a slot-stack spinlock left by an earlier process with this PID must be taken over");

    install_spinlock_owner(lock, earlier_instance());
    Reader replacement(directory.str(), "raw", elements);
    require(replacement.m_rs_index != keeper.m_rs_index && lock.test_owner() == 0,
        "a replacement reader with the dead reader's PID must acquire a slot and release the lock");
}

// A writer died holding the ring's ownership mutex, or a process died holding
// the lifecycle anchor's mutex, and a replacement with its PID attaches.
void ring_mutexes_left_by_earlier_process()
{
    sintra::test::Temp_ring_dir directory("recycled_pid_mutex");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Reader keeper(directory.str(), "raw", elements);

    auto& ownership = keeper.c.ownership_mutex;
    install_earlier_mutex_owner(ownership);
    const bool ownership_recovered = ownership.try_lock_for(1s);
    if (ownership_recovered) {
        ownership.unlock();
    }
    require(ownership_recovered,
        "a ring ownership mutex left by an earlier process with this PID and TID must be recovered");
    install_earlier_mutex_owner(ownership);
    {
        Writer replacement(directory.str(), "raw", elements);
    }

    auto& anchor = keeper.m_anchor->mutex;
    install_earlier_mutex_owner(anchor);
    const bool anchor_recovered = anchor.try_lock_for(1s);
    if (anchor_recovered) {
        anchor.unlock();
    }
    require(anchor_recovered,
        "a lifecycle-anchor mutex left by an earlier process with this PID and TID must be recovered");
    install_earlier_mutex_owner(anchor);
    Reader replacement(directory.str(), "raw", elements);
}

// A writer and a reader of one ring map its control block and lifecycle
// anchor separately. A lock that this process holds through one mapping
// excludes through the other, and is never taken over.
void locks_held_through_another_mapping()
{
    sintra::test::Temp_ring_dir directory("recycled_pid_mappings");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader reader(directory.str(), "raw", elements);
    require(
        static_cast<const void*>(&writer.c) != static_cast<const void*>(&reader.c) &&
        writer.m_anchor != reader.m_anchor,
        "the writer and the reader must map the shared objects separately");

    {
        std::atomic<bool> locked{false};
        std::atomic<bool> released{false};
        std::thread holder([&] {
            writer.c.rs_stack_spinlock.lock();
            locked = true;
            std::this_thread::sleep_for(300ms);
            released = true;
            writer.c.rs_stack_spinlock.unlock();
        });
        while (!locked) {
            std::this_thread::yield();
        }
        reader.c.rs_stack_spinlock.lock();
        const bool excluded = released.load();
        reader.c.rs_stack_spinlock.unlock();
        holder.join();
        require(excluded, "a spinlock held through another mapping must exclude until its release");
    }

    // The writer holds the ownership mutex on this thread.
    auto& ownership = reader.c.ownership_mutex;
    const auto writer_owner = ownership.test_owner_token();
    bool other_thread_acquired = true;
    std::thread contender([&] { other_thread_acquired = ownership.try_lock_for(50ms); });
    contender.join();
    const bool this_thread_acquired = ownership.try_lock();
    require(
        !other_thread_acquired && !this_thread_acquired &&
        ownership.test_owner_token() == writer_owner,
        "a mutex held through another mapping must neither be recovered nor acquired");

    auto& anchor = reader.m_anchor->mutex;
    std::atomic<bool> locked{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        writer.m_anchor->mutex.lock();
        locked = true;
        while (!release) {
            std::this_thread::yield();
        }
        writer.m_anchor->mutex.unlock();
    });
    while (!locked) {
        std::this_thread::yield();
    }
    const bool acquired_while_held = anchor.try_lock_for(50ms);
    release = true;
    holder.join();
    const bool acquired_after_release = anchor.try_lock();
    if (acquired_after_release) {
        anchor.unlock();
    }
    require(!acquired_while_held && acquired_after_release,
        "an anchor mutex held through another mapping must exclude until its release");
}

#ifndef _WIN32
struct Fork_shared
{
    sintra::spinlock  lock;
    std::atomic<bool> parent_released{false};
};

// Exit status of a fork child that checks its process instance: zero when it
// started without one and drew its own under its PID. A drawn token equal to
// an ancestor's is an independent collision, which the owner contract allows.
int check_fork_child_instance()
{
    if (sintra::detail::cached_process_instance().load() != 0) {
        return 1;
    }
    const uint64_t instance = self_instance();
    if (sintra::detail::process_instance_pid(instance) != static_cast<uint32_t>(::getpid())) {
        return 2;
    }
    return 0;
}

int wait_for_child(pid_t child)
{
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

// A fork child must not record ownership under its parent's token: an earlier
// child with its PID would then look like itself.
void fork_child_draws_own_instance()
{
    (void)self_instance();
    void* memory = ::mmap(
        nullptr,
        sizeof(Fork_shared),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0);
    require(memory != MAP_FAILED, "mmap failed");
    auto* shared = new (memory) Fork_shared();
    shared->lock.lock();

    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        const int instance_status = check_fork_child_instance();
        if (instance_status != 0) {
            ::_exit(instance_status);
        }
        shared->lock.lock();
        const bool excluded = shared->parent_released.load();
        const bool recorded = shared->lock.test_owner() == self_instance();
        shared->lock.unlock();
        if (!excluded) {
            ::_exit(3);
        }
        ::_exit(recorded ? 0 : 4);
    }

    std::this_thread::sleep_for(200ms);
    shared->parent_released = true;
    shared->lock.unlock();
    const int status = wait_for_child(child);
    shared->~Fork_shared();
    ::munmap(memory, sizeof(Fork_shared));
    require(status == 0,
        "a fork child must draw its own process instance and wait for the parent's lock");
}

// A fork child that never uses Sintra forks again. The grandchild can receive
// this process's PID after this process exits, so it must not start with the
// instance that both inherited.
void fork_grandchild_draws_own_instance()
{
    (void)self_instance();
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        const pid_t grandchild = ::fork();
        if (grandchild < 0) {
            ::_exit(5);
        }
        if (grandchild == 0) {
            ::_exit(check_fork_child_instance());
        }
        ::_exit(wait_for_child(grandchild));
    }
    require(wait_for_child(child) == 0,
        "a fork grandchild must start without its ancestors' process instance");
}
#endif

} // namespace

int main()
{
    ring_spinlock_left_by_earlier_process();
    std::puts("PASS ring_spinlock_left_by_earlier_process");
    ring_mutexes_left_by_earlier_process();
    std::puts("PASS ring_mutexes_left_by_earlier_process");
    locks_held_through_another_mapping();
    std::puts("PASS locks_held_through_another_mapping");
#ifndef _WIN32
    fork_child_draws_own_instance();
    std::puts("PASS fork_child_draws_own_instance");
    fork_grandchild_draws_own_instance();
    std::puts("PASS fork_grandchild_draws_own_instance");
#endif
    return 0;
}
