// Opt-in component measurements for P11. No timing or resource-count threshold
// is a correctness oracle; use the printed paired results as local evidence.
#include <sintra/rings.h>

#include "test_environment.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Word = uint32_t;

struct Probe_directory
{
    explicit Probe_directory(const char* hint)
        : path(sintra::test::unique_scratch_directory(hint, true)) {}
    ~Probe_directory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::string str() const { return path.string(); }
    const std::filesystem::path path;
};

size_t ring_capacity()
{
    // system_page_size() returns Windows mapping granularity on that platform.
    return sintra::system_page_size() / sizeof(Word);
}

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

unsigned bounded_number(const char* text, unsigned maximum)
{
    const std::string input(text);
    size_t used = 0;
    const unsigned long value = std::stoul(input, &used);
    if (used != input.size() || value == 0 || value > maximum) {
        throw std::invalid_argument("probe argument outside its documented bounds");
    }
    return static_cast<unsigned>(value);
}

struct Measurement
{
    uint64_t writer_cpu_ns = 0;
    uint64_t writer_wall_ns = 0;
    uint64_t evictions = 0;
    uint64_t delivered = 0;
    bool cpu_available = true;
    std::exception_ptr failure;
};

void run_round(bool hold_guard, unsigned round, Measurement& measured)
{
    Probe_directory directory("writer_stall_probe");
    const auto capacity = ring_capacity();
    const auto chunk = capacity / 8;
    sintra::Ring_W<Word> writer(directory.str(), "ring", capacity);
    sintra::Ring_R<Word> reader(directory.str(), "ring", capacity, capacity / 2);
    std::vector<Word> payload(chunk, 41);
    writer.write_commit(payload.data(), payload.size());
    const auto initial = reader.start_reading(1);
    require(initial.end - initial.begin == 1 && *initial.begin == 41,
        "initial snapshot payload mismatch");
    if (!hold_guard) {
        reader.done_reading();
    }

    sintra::detail::spinlock_cpu_sample cpu_before{}, cpu_after{};
    const bool before_available = sintra::detail::read_native_thread_cpu(cpu_before);
    const auto before = Clock::now();
    // Two complete laps guarantee that the held trailing guard obstructs an
    // octile advance. Never dereference the original snapshot after publication.
    for (unsigned i = 0; i != 16; ++i) {
        writer.write_commit(payload.data(), payload.size());
    }
    const auto after = Clock::now();
    const bool after_available = sintra::detail::read_native_thread_cpu(cpu_after);
    measured.cpu_available &= before_available && after_available;
    if (before_available && after_available) {
        require(cpu_after.ns >= cpu_before.ns, "thread CPU counter regressed");
        measured.writer_cpu_ns += cpu_after.ns - cpu_before.ns;
    }
    measured.writer_wall_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
    const auto diagnostics = writer.get_diagnostics();
    require(diagnostics.reader_eviction_count == (hold_guard ? 1u : 0u),
        "unexpected reader eviction count");
    require(diagnostics.guard_accounting_mismatch_count == 0,
        "guard accounting mismatch");
    measured.evictions += diagnostics.reader_eviction_count;

    if (hold_guard) {
        // This consumes the eviction and resumes the SAME reader at the head.
        reader.done_reading_new_data();
        require(reader.consume_eviction_notification(), "missing eviction notification");
    }
    else {
        reader.start_reading(0);
    }
    const Word marker = 1000 + round;
    writer.write_commit(marker);
    const auto resumed = reader.wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
    require(resumed.end - resumed.begin == 1 && *resumed.begin == marker,
        "post-stall marker was not delivered to the original reader");
    ++measured.delivered;
    reader.done_reading_new_data();
    reader.done_reading();
}

void stall_trial(bool hold_guard, unsigned workers, unsigned rounds)
{
#if !SINTRA_ENABLE_SLOW_READER_EVICTION
    throw std::runtime_error("stall probe requires the normal slow-reader eviction policy");
#endif
    std::vector<Measurement> results(workers);
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};
    // Release and join already-created threads even if a later creation fails.
    struct Join_threads
    {
        std::vector<std::thread>& threads;
        std::atomic<bool>& start;
        ~Join_threads()
        {
            start.store(true, std::memory_order_release);
            for (auto& thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        }
    } cleanup{threads, start};
    threads.reserve(workers);
    for (unsigned i = 0; i != workers; ++i) {
        threads.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (unsigned round = 0; round != rounds; ++round) {
                    run_round(hold_guard, round, results[i]);
                }
            }
            catch (...) {
                results[i].failure = std::current_exception();
            }
        });
    }
    const auto before = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
    Measurement total;
    for (const auto& result : results) {
        if (result.failure) {
            std::rethrow_exception(result.failure);
        }
        total.cpu_available &= result.cpu_available;
        total.writer_cpu_ns += result.writer_cpu_ns;
        total.writer_wall_ns += result.writer_wall_ns;
        total.evictions += result.evictions;
        total.delivered += result.delivered;
    }
    std::cout << "mode=" << (hold_guard ? "held_guard" : "released_guard")
              << " workers=" << workers << " rounds=" << rounds
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " trial_wall_ms=" << elapsed
              << " summed_writer_wall_ms=" << total.writer_wall_ns / 1e6
              << " cpu_available=" << total.cpu_available
              << " summed_writer_cpu_ms=" << total.writer_cpu_ns / 1e6
              << " evictions=" << total.evictions
              << " post_stall_delivered=" << total.delivered << '\n';
}

#if defined(_WIN32) && !defined(SINTRA_BACKEND_POLLING)
struct Handle_counts
{
    DWORD process = 0;
    size_t cached_names = 0;
};

Handle_counts handle_counts()
{
    Handle_counts result;
    require(::GetProcessHandleCount(::GetCurrentProcess(), &result.process) != FALSE,
        "GetProcessHandleCount failed");
    auto& cache = sintra::detail::ips_win_handles();
    std::shared_lock<sintra::shared_mutex> lock(cache.m);
    result.cached_names = cache.map.size();
    for (const auto& entry : cache.map) {
        DWORD flags = 0;
        require(::GetHandleInformation(entry.second, &flags) != FALSE,
            "cache contains an invalid handle");
    }
    return result;
}

void print_handles(const char* phase, Handle_counts before, Handle_counts after, unsigned rounds)
{
    std::cout << "mode=" << phase << " rounds=" << rounds
              << " process_handles_before=" << before.process
              << " process_handles_after=" << after.process
              << " process_handle_delta=" << int64_t(after.process) - before.process
              << " cached_names_before=" << before.cached_names
              << " cached_names_after=" << after.cached_names
              << " cached_name_delta=" << int64_t(after.cached_names) - before.cached_names
              << '\n';
}

void handle_retention(unsigned rounds)
{
    Probe_directory directory("semaphore_handles");
    const auto capacity = ring_capacity();
    auto attach = [&](sintra::Ring_W<Word>& writer, const std::string& name) {
        sintra::Ring_R<Word> reader(directory.str(), name, capacity, capacity / 2);
        // Admission has prepared the native semaphore before publishing its
        // slot. This mode deliberately excludes timer-thread and process noise.
        writer.write_commit(Word{73});
        auto snapshot = sintra::make_snapshot(reader, 1);
        require(snapshot.end() - snapshot.begin() == 1 && *snapshot.begin() == 73,
            "attach/detach payload mismatch");
    };
    {
        sintra::Ring_W<Word> writer(directory.str(), "reused", capacity);
        attach(writer, "reused");
        const auto before = handle_counts();
        for (unsigned i = 0; i != rounds; ++i) {
            attach(writer, "reused");
        }
        print_handles("reused_ring", before, handle_counts(), rounds);
    }
    const auto before = handle_counts();
    for (unsigned i = 0; i != rounds; ++i) {
        const auto name = "fresh_" + std::to_string(i);
        sintra::Ring_W<Word> writer(directory.str(), name, capacity);
        attach(writer, name);
    }
    print_handles("fresh_rings", before, handle_counts(), rounds);
}
#endif

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--stall" && argc <= 4) {
            const unsigned workers = argc >= 3 ? bounded_number(argv[2], 32) : 1;
            const unsigned rounds = argc >= 4 ? bounded_number(argv[3], 100) : 10;
            // Paired order is reversed on the second pass to expose order bias.
            stall_trial(false, workers, rounds);
            stall_trial(true, workers, rounds);
            stall_trial(true, workers, rounds);
            stall_trial(false, workers, rounds);
            return 0;
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--handle-retention" && argc <= 3) {
#if defined(_WIN32) && !defined(SINTRA_BACKEND_POLLING)
            handle_retention(argc == 3 ? bounded_number(argv[2], 1000) : 64);
            return 0;
#else
            std::cerr << "UNAVAILABLE: --handle-retention requires the native Windows backend\n";
            return 77;
#endif
        }
        std::cerr << "Usage: ring_resource_probe --stall [workers:1..32] [rounds:1..100]\n"
                  << "       ring_resource_probe --handle-retention [rounds:1..1000]\n";
        return 2;
    }
    catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
