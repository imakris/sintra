#include <sintra/sintra.h>

#include "../tests/test_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct settings_t
{
    unsigned readers = 1;
    unsigned messages = 20000;
    unsigned warmup = 1000;
    unsigned burst_messages = 0;
    size_t bytes = 64;
};

settings_t settings;
std::atomic<uint64_t> evictions{0};

uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

double cpu_seconds()
{
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    const auto ticks = [](FILETIME value) {
        return ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) / 1.0e7;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("getrusage failed");
    }
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
        (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1.0e6;
#endif
}

void log_message(sintra::log_level level, const char* message, void*)
{
    if (std::string_view(message).find("evict") != std::string_view::npos) {
        evictions.fetch_add(1);
    }
    if (level == sintra::log_level::error) {
        std::fprintf(stderr, "%s\n", message);
    }
}

double percentile(std::vector<uint64_t>& values, double fraction)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[(size_t)(fraction * (values.size() - 1))] / 1000.0;
}

void processing_barrier(const char* name)
{
    sintra::barrier<sintra::processing_fence_t>(name, "_sintra_all_processes");
}

void receive_batches(unsigned messages, const char* name)
{
    if (settings.burst_messages != 0) {
        for (unsigned completed = settings.burst_messages; completed <= messages;
             completed += settings.burst_messages)
        {
            processing_barrier(name);
        }
    }
}

int receiver()
{
    sintra::test::Shared_directory directory("SINTRA_BENCHMARK_DIR", "broadcast");
    std::vector<uint64_t> latencies;
    latencies.reserve(settings.messages);
    uint64_t received = 0;
    uint64_t errors = 0;
    double first_cpu = 0;
    double last_cpu = 0;
    sintra::activate_slot([&](const std::string& message) {
        uint64_t sequence = 0;
        uint64_t sent = 0;
        if (message.size() != settings.bytes) {
            ++errors;
            return;
        }
        std::memcpy(&sequence, message.data(), sizeof(sequence));
        std::memcpy(&sent, message.data() + sizeof(sequence), sizeof(sent));
        if (sequence == UINT64_MAX) {
            return;
        }
        if (received == 0) {
            first_cpu = cpu_seconds();
        }
        if (sequence != received || message.back() != 'x') {
            ++errors;
        }
        latencies.push_back(now_ns() - sent);
        ++received;
        if (received == settings.messages) {
            last_cpu = cpu_seconds();
        }
    });
    processing_barrier("benchmark-ready");
    receive_batches(settings.warmup, "benchmark-warm-burst");
    processing_barrier("benchmark-warm");
    receive_batches(settings.messages, "benchmark-burst");
    processing_barrier("benchmark-complete");
    const auto file = directory.path() / (std::to_string(sintra::test::get_pid()) + ".reader");
    std::ofstream output(file);
    output << received << ' ' << errors << ' ' << (last_cpu - first_cpu) << ' '
           << evictions.load() << '\n';
    for (const auto latency : latencies) {
        output << latency << '\n';
    }
    output.close();
    processing_barrier("benchmark-results");
    return 0;
}

int publish(const std::filesystem::path& directory)
{
    std::string message(settings.bytes, 'x');
    auto send = [&](uint64_t sequence) {
        const auto sent = now_ns();
        std::memcpy(message.data(), &sequence, sizeof(sequence));
        std::memcpy(message.data() + sizeof(sequence), &sent, sizeof(sent));
        sintra::world() << message;
    };
    processing_barrier("benchmark-ready");
    for (unsigned i = 0; i < settings.warmup; ++i) {
        send(UINT64_MAX);
        if (settings.burst_messages != 0 && (i + 1) % settings.burst_messages == 0) {
            processing_barrier("benchmark-warm-burst");
        }
    }
    processing_barrier("benchmark-warm");
    evictions = 0;
    std::vector<uint64_t> calls;
    std::vector<uint64_t> drains;
    calls.reserve(settings.messages);
    drains.reserve(settings.burst_messages != 0 ? settings.messages / settings.burst_messages : 0);
    const auto cpu_begin = cpu_seconds();
    const auto begin = now_ns();
    for (unsigned i = 0; i < settings.messages; ++i) {
        const auto call_begin = now_ns();
        send(i);
        calls.push_back(now_ns() - call_begin);
        if (settings.burst_messages != 0 && (i + 1) % settings.burst_messages == 0) {
            const auto drain_begin = now_ns();
            processing_barrier("benchmark-burst");
            drains.push_back(now_ns() - drain_begin);
        }
    }
    processing_barrier("benchmark-complete");
    const auto elapsed = (now_ns() - begin) / 1.0e9;
    const auto writer_cpu = cpu_seconds() - cpu_begin;
    processing_barrier("benchmark-results");
    uint64_t delivered = 0;
    uint64_t errors = 0;
    uint64_t losses = 0;
    uint64_t eviction_count = evictions.load();
    unsigned readers = 0;
    double reader_cpu = 0;
    std::vector<uint64_t> latencies;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".reader") {
            continue;
        }
        std::ifstream input(entry.path());
        uint64_t count = 0;
        uint64_t bad = 0;
        uint64_t reader_evictions = 0;
        double cpu = 0;
        input >> count >> bad >> cpu >> reader_evictions;
        if (!input) {
            throw std::runtime_error("incomplete benchmark reader report");
        }
        ++readers;
        delivered += count;
        errors += bad;
        losses += count < settings.messages ? settings.messages - count : 0;
        reader_cpu += cpu;
        eviction_count += reader_evictions;
        uint64_t latency = 0;
        while (input >> latency) {
            latencies.push_back(latency);
        }
    }
    const bool complete = readers == settings.readers && errors == 0 && losses == 0 &&
        delivered == (uint64_t)settings.readers * settings.messages;
    std::printf(
        "{\"readers\":%u,\"payload_bytes\":%zu,\"messages\":%u,\"delivered\":%llu,"
        "\"seconds\":%.9f,\"delivered_per_second\":%.3f,\"bytes_per_second\":%.3f,"
        "\"writer_cpu_seconds\":%.6f,\"reader_cpu_seconds\":%.6f,"
        "\"latency_p50_us\":%.3f,\"latency_p95_us\":%.3f,\"latency_p99_us\":%.3f,"
        "\"latency_max_us\":%.3f,\"broadcast_p99_us\":%.3f,\"broadcast_max_us\":%.3f,"
        "\"burst_messages\":%u,\"drain_p99_us\":%.3f,\"drain_max_us\":%.3f,"
        "\"eviction_reports\":%llu,\"losses\":%llu,\"errors\":%llu,\"complete\":%s}\n",
        settings.readers, settings.bytes, settings.messages, (unsigned long long)delivered,
        elapsed, delivered / elapsed, delivered * settings.bytes / elapsed,
        writer_cpu, reader_cpu, percentile(latencies, 0.50), percentile(latencies, 0.95),
        percentile(latencies, 0.99), percentile(latencies, 1.0), percentile(calls, 0.99),
        percentile(calls, 1.0), settings.burst_messages, percentile(drains, 0.99),
        percentile(drains, 1.0), (unsigned long long)eviction_count,
        (unsigned long long)losses, (unsigned long long)errors, complete ? "true" : "false");
    return complete ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        settings.readers = (unsigned)std::stoul(
            sintra::test::get_argv_value(argc, argv, "--readers", "1"));
        settings.messages = (unsigned)std::stoul(
            sintra::test::get_argv_value(argc, argv, "--messages", "20000"));
        settings.warmup = (unsigned)std::stoul(
            sintra::test::get_argv_value(argc, argv, "--warmup", "1000"));
        settings.bytes = std::stoull(
            sintra::test::get_argv_value(argc, argv, "--bytes", "64"));
        if (settings.readers == 0 || settings.readers > 64 || settings.bytes < 17 ||
            settings.bytes > sintra::detail::message_frame_size_limit - 512)
        {
            throw std::invalid_argument("readers must be 1..64; payload must be 17..frame-limit-512");
        }
        // Reserve framing headroom and drain before a burst can wrap the ring.
        // Both revisions include the same public processing-fence cost.
        const auto default_burst = (sintra::message_ring_size / 4) / (settings.bytes + 512);
        settings.burst_messages = (unsigned)std::stoul(sintra::test::get_argv_value(
            argc, argv, "--burst", std::to_string(std::max<size_t>(1, default_burst))));
        sintra::set_log_callback(&log_message, nullptr);
        std::vector<sintra::Process_descriptor> processes(settings.readers, receiver);
        const std::vector<std::string> child_options = {
            "--readers", std::to_string(settings.readers),
            "--messages", std::to_string(settings.messages),
            "--warmup", std::to_string(settings.warmup),
            "--burst", std::to_string(settings.burst_messages),
            "--bytes", std::to_string(settings.bytes),
        };
        for (auto& process : processes) {
            process.user_options = child_options;
        }
        return sintra::test::run_multi_process_shutdown_test(
            argc, argv, "SINTRA_BENCHMARK_DIR", "broadcast", std::move(processes),
            publish, [](const std::filesystem::path&) { return 0; });
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "broadcast_benchmark: %s\n", error.what());
        return 1;
    }
}
