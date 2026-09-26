#include <sintra/rings.h>

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using word = uint32_t;
constexpr const char* k_ring_name = "wakeup_ownership";
constexpr word k_payload = 0x51a7;
constexpr auto k_deadline = std::chrono::seconds(8);

struct Probe_writer : sintra::Ring_W<word>
{
    using sintra::Ring_W<word>::Ring_W;
    using sintra::Ring_W<word>::m_control;

    bool no_pending_wakeups()
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        if (m_control->sleeping_stack.size() != 0) {
            return false;
        }
        for (auto& semaphore : m_control->dirty_semaphores) {
            if (semaphore.wait_for(std::chrono::nanoseconds(0)) !=
                sintra::sintra_ring_semaphore::wait_result::timeout)
            {
                return false;
            }
        }
        return true;
    }
};

struct Park_gate
{
    std::mutex mutex;
    std::condition_variable changed;
    std::array<bool, sintra::max_process_index> indices{};
    size_t prepared = 0;
    bool duplicate = false;
    bool released = false;

    void prepare(int index)
    {
        std::unique_lock lock(mutex);
        if (index < 0 || index >= sintra::max_process_index || indices[index]) {
            duplicate = true;
        }
        else {
            indices[index] = true;
            ++prepared;
        }
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }

    bool wait(size_t count)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, k_deadline, [&] { return prepared == count; }) &&
            !duplicate;
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};

Park_gate* s_park_gate = nullptr;

void pause_before_wait(int index)
{
    s_park_gate->prepare(index);
}

struct Parked_readers
{
    Park_gate gate;
    std::vector<std::unique_ptr<sintra::Ring_R<word>>> readers;
    std::vector<std::thread> threads;
    std::atomic<size_t> returned{0};
    std::atomic<size_t> received{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stop_requested{false};

    void start(const std::string& directory, size_t capacity, size_t count)
    {
        s_park_gate = &gate;
        sintra::detail::test_hooks::s_ring_wait_prepared = &pause_before_wait;
        for (size_t i = 0; i != count; ++i) {
            readers.push_back(std::make_unique<sintra::Ring_R<word>>(
                directory, k_ring_name, capacity));
        }
        for (auto& reader : readers) {
            threads.emplace_back([&, reader = reader.get()] {
                try {
                    reader->start_reading();
                    while (!stop_requested.load()) {
                        const auto range = reader->wait_for_new_data();
                        if (range.begin && range.end != range.begin) {
                            if (range.end - range.begin == 1 && *range.begin == k_payload) {
                                ++received;
                            }
                            else {
                                failed = true;
                            }
                            break;
                        }
                    }
                    reader->done_reading();
                    ++returned;
                }
                catch (...) {
                    failed = true;
                }
            });
        }
    }

    void stop()
    {
        stop_requested = true;
        for (auto& reader : readers) {
            reader->request_stop();
        }
        gate.release();
    }

    void join()
    {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    ~Parked_readers()
    {
        stop();
        join();
        sintra::detail::test_hooks::s_ring_wait_prepared = nullptr;
        s_park_gate = nullptr;
    }
};

bool stop_preserves_capacity()
{
    sintra::test::Temp_ring_dir directory("wakeup_stop");
    const size_t capacity = sintra::test::pick_ring_elements<word>();
    Probe_writer writer(directory.str(), k_ring_name, capacity);
    {
        Parked_readers stopped;
        stopped.start(directory.str(), capacity, 1);
        if (!stopped.gate.wait(1)) {
            std::fprintf(stderr, "Initial reader did not prepare its wait\n");
            return false;
        }
        // Stop exactly after registration, before the semaphore wait starts.
        stopped.stop();
        stopped.join();
        if (stopped.failed || stopped.returned != 1) {
            return false;
        }
    }
    writer.unblock_global();
    if (!writer.no_pending_wakeups()) {
        std::fprintf(stderr, "Stopped reader left a wakeup for its successor\n");
        return false;
    }

    Parked_readers full;
    full.start(directory.str(), capacity, sintra::max_process_index);
    const bool all_prepared = full.gate.wait(sintra::max_process_index);
    full.stop();
    full.join();
    if (!all_prepared || full.failed || full.returned != sintra::max_process_index) {
        std::fprintf(stderr, "Stopped reader reduced wakeup capacity: prepared=%zu expected=%d\n",
            full.gate.prepared, int(sintra::max_process_index));
        return false;
    }
    return writer.no_pending_wakeups();
}

bool dead_reader_capacity_is_recovered(const std::string& binary)
{
    sintra::test::Temp_ring_dir directory("wakeup_dead_child");
    const size_t capacity = sintra::test::pick_ring_elements<word>();
    Probe_writer writer(directory.str(), k_ring_name, capacity);
    sintra::test::Exact_child child(std::chrono::seconds(5));
    const std::string path = directory.str();
    const char* args[] = {binary.c_str(), "--wakeup-child", path.c_str(), nullptr};
    if (!child.spawn(binary.c_str(), args)) {
        std::fprintf(stderr, "Cannot launch owned child: %s\n", child.error().c_str());
        return false;
    }

    bool parked = false;
    const auto deadline = std::chrono::steady_clock::now() + k_deadline;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            sintra::spinlock::locker lock(writer.m_control->m_spinlock);
            parked = writer.m_control->sleeping_stack.size() == sintra::max_process_index;
        }
        if (parked || child.poll() != sintra::test::Exact_child_state::running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::string diagnostic;
    const bool terminated = child.terminate_and_settle(diagnostic);
    if (!parked || !terminated) {
        std::fprintf(stderr, "Owned child did not park/settle: parked=%d %s\n",
            parked, diagnostic.c_str());
        return false;
    }

    // Ordered flushing used to discard all indices whose reader had died.
    writer.unblock_global();
    writer.m_control->scavenge_orphans();
    if (!writer.no_pending_wakeups()) {
        std::fprintf(stderr, "Dead reader left a wakeup for its successor\n");
        return false;
    }
    Parked_readers recovered;
    recovered.start(directory.str(), capacity, sintra::max_process_index);
    const bool prepared = recovered.gate.wait(sintra::max_process_index);
    if (!prepared) {
        recovered.stop();
        recovered.join();
        std::fprintf(stderr, "Dead readers exhausted the surviving ring's wakeups\n");
        return false;
    }
    writer.write_commit(k_payload);
    recovered.gate.release();
    recovered.join();
    return !recovered.failed && recovered.returned == sintra::max_process_index &&
        recovered.received == sintra::max_process_index && writer.no_pending_wakeups();
}

int run_child(const char* directory)
{
    Parked_readers readers;
    readers.start(directory, sintra::test::pick_ring_elements<word>(), sintra::max_process_index);
    if (!readers.gate.wait(sintra::max_process_index)) {
        return 2;
    }
    // The parent owns the exact process and terminates it at the parked seam.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--wakeup-child") {
            return run_child(argv[2]);
        }
        const bool death_only = argc > 1 && std::string_view(argv[1]) == "--death-only";
        const bool stop_only = argc > 1 && std::string_view(argv[1]) == "--stop-only";
        bool ok = true;
        if (!death_only) {
            ok = stop_preserves_capacity();
        }
        if (!stop_only) {
            ok = dead_reader_capacity_is_recovered(
                sintra::test::get_binary_path(argc, argv)) && ok;
        }
        return ok ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "ring_wakeup_ownership_test: %s\n", error.what());
        return 1;
    }
}
