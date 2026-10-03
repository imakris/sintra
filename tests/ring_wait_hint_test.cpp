#include <sintra/rings.h>

#include "test_ring_utils.h"
#include "test_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Hint = sintra::Ring_wait_hint;
using Clock = std::chrono::steady_clock;
using word = uint32_t;
constexpr auto k_deadline = std::chrono::seconds(3);
constexpr const char* k_prefix = "ring_wait_hint_test: ";

bool expect(bool condition, const char* message)
{
    return sintra::test::assert_true(condition, k_prefix, message);
}

class Wait_gate
{
public:
    void enter(Hint hint, int index = -1)
    {
        std::unique_lock lock(m_mutex);
        if (!m_entered) {
            m_hint = hint;
            m_index = index;
            m_entered = true;
            m_changed.notify_all();
        }
        m_changed.wait(lock, [&] { return m_released; });
    }

    bool wait()
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, k_deadline, [&] { return m_entered; });
    }

    void release()
    {
        std::lock_guard lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

    Hint m_hint = Hint::BLOCKING;
    int m_index = -1;

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_entered = false;
    bool m_released = false;
};

Wait_gate* s_wait_gate = nullptr;

void gate_started(Hint hint) { s_wait_gate->enter(hint); }
void gate_prepared(int index) { s_wait_gate->enter(Hint::BLOCKING, index); }

class Wait_hooks
{
public:
    Wait_hooks(Wait_gate& gate, bool prepared)
    {
        s_wait_gate = &gate;
        if (prepared) {
            sintra::detail::test_hooks::s_ring_wait_prepared = &gate_prepared;
        }
        else {
            sintra::detail::test_hooks::s_ring_wait_started = &gate_started;
        }
    }

    ~Wait_hooks()
    {
        sintra::detail::test_hooks::s_ring_wait_started = nullptr;
        sintra::detail::test_hooks::s_ring_wait_prepared = nullptr;
        s_wait_gate = nullptr;
    }
};

class Probe_reader : public sintra::Ring_R<word>
{
public:
    using sintra::Ring_R<word>::Ring_R;

    bool no_pending_wakeups()
    {
        sintra::spinlock::locker lock(this->m_control->m_spinlock);
        if (this->m_control->sleeping_stack.size() != 0) {
            return false;
        }
        for (auto& semaphore : this->m_control->dirty_semaphores) {
            if (semaphore.wait_for(std::chrono::nanoseconds(0)) !=
                sintra::sintra_ring_semaphore::wait_result::timeout)
            {
                return false;
            }
        }
        return true;
    }
};

bool default_and_pending_data()
{
    bool ok = true;
    for (int mode = 0; mode != 3; ++mode) {
        sintra::test::Temp_ring_dir directory("wait_hint_pending");
        const auto capacity = sintra::test::pick_ring_elements<word>();
        sintra::Ring_W<word> writer(directory.str(), "pending", capacity);
        Probe_reader reader(directory.str(), "pending", capacity);
        reader.start_reading();
        const word values[] = {17, 19, 23};
        writer.write_commit(values, 3);
        Wait_gate gate;
        Wait_hooks hooks(gate, false);
        std::exception_ptr error;
        bool delivered = false;
        std::thread worker([&] {
            try {
                const auto range = mode == 0 ? reader.wait_for_new_data()
                    : reader.wait_for_new_data(mode == 1 ? Hint::ADAPTIVE : Hint::BLOCKING);
                delivered = range.begin && range.end - range.begin == 3 &&
                    std::equal(range.begin, range.end, values);
                reader.done_reading();
            }
            catch (...) {
                error = std::current_exception();
            }
        });
        const bool entered = gate.wait();
        ok &= expect(entered && gate.m_hint == (mode == 2 ? Hint::BLOCKING : Hint::ADAPTIVE),
            "default and explicit wait hints must select their requested policy");
        if (!entered) {
            reader.request_stop();
        }
        gate.release();
        worker.join();
        ok &= expect(delivered && !error && reader.no_pending_wakeups(),
            "every policy must deliver already committed data in order");
    }
    return ok;
}

enum class wake_action { publish, stop, unblock, final_close };

bool registered_wakes(wake_action action)
{
    sintra::test::Temp_ring_dir directory("wait_hint_wakes");
    const auto capacity = sintra::test::pick_ring_elements<word>();
    auto writer = std::make_unique<sintra::Ring_W<word>>(directory.str(), "wakes", capacity);
    Probe_reader reader(directory.str(), "wakes", capacity);
    reader.start_reading();
    Wait_gate gate;
    Wait_hooks hooks(gate, true);
    std::exception_ptr error;
    bool delivered = false;
    bool empty = false;
    bool closed = false;
    std::thread worker([&] {
        try {
            const auto range = reader.wait_for_new_data(Hint::BLOCKING);
            empty = !range.begin || range.begin == range.end;
            delivered = range.begin && range.end - range.begin == 1 && *range.begin == 31;
            if (action == wake_action::final_close && delivered) {
                reader.done_reading_new_data();
                const auto last = reader.wait_for_new_data(Hint::BLOCKING);
                closed = (!last.begin || last.begin == last.end) && reader.is_stopping();
            }
            reader.done_reading();
        }
        catch (...) {
            error = std::current_exception();
        }
    });
    const bool registered = gate.wait();
    if (!registered) {
        reader.request_stop();
    }
    else {
        switch (action) {
        case wake_action::publish:
            writer->write_commit(word(31));
            break;
        case wake_action::stop:
            reader.request_stop();
            break;
        case wake_action::unblock:
            writer->unblock_global();
            break;
        case wake_action::final_close:
            writer->write_commit(word(31));
            writer.reset();
            break;
        }
    }
    gate.release();
    worker.join();
    return expect(registered && gate.m_index >= 0 && !error && reader.no_pending_wakeups() &&
        ((action == wake_action::publish && delivered) ||
         (action == wake_action::final_close && delivered && closed) ||
         ((action == wake_action::stop || action == wake_action::unblock) && empty)),
        "publication, final close, stop and explicit unblock must preserve registered wakeups");
}

struct frame_t { uint64_t sequence; };
using Frame = sintra::Message<frame_t, void, 0x71A4E03ull>;

void write_frame(sintra::Message_ring_W& writer, uint64_t sequence)
{
    writer.write<Frame>(sintra::vb_size<Frame>(sequence), sequence);
    writer.done_writing();
}

bool batch_hint_lifetime()
{
    sintra::test::Temp_ring_dir directory("wait_hint_batch");
    sintra::Message_ring_W writer(directory.str(), "req", 1);
    sintra::Message_ring_R reader(directory.str(), "req", 1);
    reader.start_reading();
    Hint hint = Hint::BLOCKING;
    write_frame(writer, 1);
    auto* first = static_cast<Frame*>(reader.fetch_message(&hint));
    bool ok = expect(first && first->sequence == 1, "first frame must be delivered");
    hint = Hint::ADAPTIVE;
    write_frame(writer, 2);
    auto* second = static_cast<Frame*>(reader.fetch_message(&hint));
    ok &= expect(second && second->sequence == 2 && hint == Hint::ADAPTIVE,
        "useful work must survive another immediately readable range");
    Wait_gate gate;
    Wait_hooks hooks(gate, false);
    std::exception_ptr error;
    bool delivered = false;
    std::thread worker([&] {
        try {
            auto* third = static_cast<Frame*>(reader.fetch_message(&hint));
            delivered = third && third->sequence == 3;
            reader.done_reading();
        }
        catch (...) {
            error = std::current_exception();
        }
    });
    const bool entered = gate.wait();
    ok &= expect(entered && gate.m_hint == Hint::ADAPTIVE && hint == Hint::BLOCKING,
        "an observed-empty boundary must consume the batch hint and reset the next one");
    if (entered) {
        write_frame(writer, 3);
    }
    else {
        reader.request_stop();
    }
    gate.release();
    worker.join();
    return expect(delivered && !error, "publication after an empty observation must remain deliverable") && ok;
}

class Target : public sintra::Derived_transceiver<Target>
{
public:
    void touch(uint64_t value)
    {
        if (value != 0) {
            ++m_received;
        }
    }
    SINTRA_UNICAST(touch)
    std::atomic<unsigned> m_received{0};
};

class Managed_probe
{
public:
    explicit Managed_probe(const std::filesystem::path& directory) : m_directory(directory) {}

    void observe(Hint hint)
    {
        auto* reader = sintra::s_tl_current_request_reader;
        if (!sintra::tl_is_req_thread || !reader ||
            reader->get_process_instance_id() != sintra::process_of(sintra::s_coord_id))
        {
            return;
        }
        std::lock_guard lock(m_mutex);
        m_records.emplace_back(reader->get_request_reading_sequence(), hint);
        m_changed.notify_all();
    }

    void prepare()
    {
        if (!sintra::tl_is_req_thread || sintra::s_coord || !m_gate_armed.exchange(false)) {
            return;
        }
        std::ofstream(m_directory / ("parked_" + std::to_string(sintra::process_index()))) << 1;
        uint64_t sequence = 0;
        if (!read_number("release", sequence)) {
            m_failed = true;
        }
        m_expected_sequence = sequence;
        m_changed.notify_all();
    }

    bool read_number(const std::string& name, uint64_t& value)
    {
        const auto deadline = Clock::now() + k_deadline;
        do {
            std::ifstream input(m_directory / name);
            if (input >> value) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (Clock::now() < deadline);
        return false;
    }

    bool selected(uint64_t sequence, Hint hint)
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, k_deadline, [&] {
            return m_failed || std::find(m_records.begin(), m_records.end(),
                std::pair{sequence, hint}) != m_records.end();
        }) && !m_failed;
    }

    bool selected_expected(Hint hint)
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, k_deadline, [&] {
            const auto sequence = m_expected_sequence.load();
            return m_failed || (sequence != 0 && std::find(m_records.begin(), m_records.end(),
                std::pair{sequence, hint}) != m_records.end());
        }) && !m_failed;
    }

    std::filesystem::path m_directory;
    std::atomic<bool> m_gate_armed{false};

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::pair<uint64_t, Hint>> m_records;
    std::atomic<uint64_t> m_expected_sequence{0};
    std::atomic<bool> m_failed{false};
};

Managed_probe* s_managed_probe = nullptr;
void managed_started(Hint hint) { s_managed_probe->observe(hint); }
void managed_prepared(int) { s_managed_probe->prepare(); }

void start_managed_observation()
{
    sintra::detail::test_hooks::s_ring_wait_started = &managed_started;
    sintra::detail::test_hooks::s_ring_wait_prepared = &managed_prepared;
}

int managed_child()
{
    Target target;
    auto& probe = *s_managed_probe;
    const int branch = sintra::process_index();
    std::ofstream(probe.m_directory / ("receiver_" + std::to_string(branch))) << target.instance_id();
    sintra::barrier<sintra::processing_fence_t>("wait-hint-ready", "_sintra_all_processes");
    probe.m_gate_armed = true;
    start_managed_observation();
    const bool selected = probe.selected_expected(branch == 3 ? Hint::BLOCKING : Hint::ADAPTIVE);
    std::ofstream(probe.m_directory / ("result_" + std::to_string(branch)))
        << selected << ' ' << target.m_received.load();
    uint64_t finished = 0;
    probe.read_number("finish", finished);
    sintra::barrier<sintra::processing_fence_t>("wait-hint-finished", "_sintra_all_processes");
    return selected ? 0 : 1;
}

bool managed_coordinator()
{
    Target sender;
    auto& probe = *s_managed_probe;
    uint64_t first = 0;
    uint64_t second = 0;
    bool ok = probe.read_number("receiver_1", first) && probe.read_number("receiver_2", second);
    if (ok) {
        Target::rpc_touch(second, 0);
    }
    sintra::barrier<sintra::processing_fence_t>("wait-hint-ready", "_sintra_all_processes");
    start_managed_observation();
    for (int branch = 1; branch != 4; ++branch) {
        uint64_t parked = 0;
        ok = probe.read_number("parked_" + std::to_string(branch), parked) && parked == 1 && ok;
    }
    uint64_t sequence = 0;
    if (ok) {
        Target::rpc_touch(first, 1);
        Target::rpc_touch(second, 2);
        sequence = sintra::s_mproc->m_out_req_c->get_leading_sequence();
    }
    std::ofstream(probe.m_directory / "release") << sequence;
    if (ok) {
        ok = probe.selected(sequence, Hint::ADAPTIVE);
    }
    for (int branch = 1; branch != 4; ++branch) {
        uint64_t selected = 0;
        ok = probe.read_number("result_" + std::to_string(branch), selected) && ok;
    }
    std::ofstream(probe.m_directory / "finish") << 1;
    sintra::barrier<sintra::processing_fence_t>("wait-hint-finished", "_sintra_all_processes");
    for (int branch = 1; branch != 4; ++branch) {
        std::ifstream result(probe.m_directory / ("result_" + std::to_string(branch)));
        bool selected = false;
        unsigned received = 0;
        const bool valid = bool(result >> selected >> received);
        ok &= expect(valid && selected && received == (branch == 3 ? 0u : 1u),
            "managed local/mixed work must stay adaptive; unrelated targeted traffic must block");
    }
    return ok;
}

int managed_test(int argc, char* argv[])
{
    sintra::test::Shared_directory directory("SINTRA_WAIT_HINT_TEST_DIR", "wait_hint_managed");
    Managed_probe probe(directory.path());
    s_managed_probe = &probe;
    struct Hook_cleanup
    {
        ~Hook_cleanup()
        {
            sintra::detail::test_hooks::s_ring_wait_started = nullptr;
            sintra::detail::test_hooks::s_ring_wait_prepared = nullptr;
            s_managed_probe = nullptr;
        }
    } cleanup;
    return sintra::test::run_multi_process_shutdown_test(
        argc, argv, "SINTRA_WAIT_HINT_TEST_DIR", "wait_hint_managed",
        std::vector<sintra::Process_descriptor>(3, managed_child),
        [](const std::filesystem::path&) { return managed_coordinator() ? 0 : 1; },
        [](const std::filesystem::path&) { return 0; });
}

struct timed_value_t { uint64_t sequence; uint64_t sent_ns; };

uint64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

uint64_t current_cycles()
{
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        throw std::runtime_error("QueryThreadCycleTime failed");
    }
    return cycles;
#else
    return 0;
#endif
}

struct Reader_measurement
{
    uint64_t received = 0;
    uint64_t cpu_ns = 0;
    uint64_t cycles = 0;
    bool cpu_available = false;
    bool valid = true;
    std::vector<uint64_t> latencies;
};

bool benchmark_policy(Hint hint, unsigned readers, unsigned seconds)
{
    sintra::test::Temp_ring_dir directory("wait_hint_benchmark");
    const uint64_t messages = seconds * 5;
    const auto capacity = sintra::test::pick_ring_elements<timed_value_t>(messages + 8);
    auto writer = std::make_unique<sintra::Ring_W<timed_value_t>>(
        directory.str(), "benchmark", capacity);
    std::vector<std::unique_ptr<sintra::Ring_R<timed_value_t>>> rings;
    for (unsigned i = 0; i != readers; ++i) {
        rings.push_back(std::make_unique<sintra::Ring_R<timed_value_t>>(
            directory.str(), "benchmark", capacity));
    }
    std::vector<Reader_measurement> results(readers);
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable ready;
    unsigned started = 0;
    struct Reader_cleanup
    {
        decltype(rings)& m_rings;
        decltype(threads)& m_threads;
        ~Reader_cleanup()
        {
            for (auto& reader : m_rings) {
                reader->request_stop();
            }
            for (auto& thread : m_threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        }
    } cleanup{rings, threads};
    for (unsigned i = 0; i != readers; ++i) {
        threads.emplace_back([&, i] {
            auto& reader = *rings[i];
            auto& result = results[i];
            try {
                reader.start_reading();
                sintra::detail::spinlock_cpu_sample before;
                const bool before_available = sintra::detail::read_spinlock_thread_cpu(before);
                const auto before_cycles = current_cycles();
                {
                    std::lock_guard lock(mutex);
                    ++started;
                    ready.notify_all();
                }
                while (result.received < messages && !reader.is_stopping()) {
                    const auto range = reader.wait_for_new_data(hint);
                    for (auto* value = range.begin; value && value != range.end; ++value) {
                        result.valid &= value->sequence == result.received + 1;
                        ++result.received;
                        result.latencies.push_back(now_ns() - value->sent_ns);
                    }
                    reader.done_reading_new_data();
                }
                sintra::detail::spinlock_cpu_sample after;
                result.cpu_available = before_available && sintra::detail::read_spinlock_thread_cpu(after);
                if (result.cpu_available) {
                    result.cpu_ns = after.ns - before.ns;
                }
                result.cycles = current_cycles() - before_cycles;
                reader.done_reading();
            }
            catch (...) {
                result.valid = false;
            }
        });
    }
    {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, k_deadline, [&] { return started == readers; })) {
            lock.unlock();
            for (auto& reader : rings) {
                reader->request_stop();
            }
            for (auto& thread : threads) {
                thread.join();
            }
            return false;
        }
    }
    const auto begin = Clock::now();
    for (uint64_t sequence = 1; sequence <= messages; ++sequence) {
        std::this_thread::sleep_until(begin + std::chrono::milliseconds(200 * sequence));
        writer->write_commit(timed_value_t{sequence, now_ns()});
    }
    writer.reset();
    for (auto& thread : threads) {
        thread.join();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    uint64_t received = 0;
    uint64_t cpu_ns = 0;
    uint64_t cycles = 0;
    bool valid = true;
    bool cpu_available = true;
    std::vector<uint64_t> latencies;
    for (auto& result : results) {
        received += result.received;
        cpu_ns += result.cpu_ns;
        cycles += result.cycles;
        valid &= result.valid && result.received == messages;
        cpu_available &= result.cpu_available;
        latencies.insert(latencies.end(), result.latencies.begin(), result.latencies.end());
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile_us = [&](double fraction) {
        return latencies.empty() ? 0.0 : latencies[size_t(fraction * (latencies.size() - 1))] / 1000.0;
    };
    const std::string cpu = cpu_available ? std::to_string(cpu_ns) : "null";
#ifdef _WIN32
    const std::string cycle_value = std::to_string(cycles);
#else
    const std::string cycle_value = "null";
#endif
    std::printf("{\"scope\":\"raw-ring\",\"hint\":\"%s\",\"readers\":%u,\"messages\":%llu,"
        "\"seconds\":%.6f,\"delivered\":%llu,\"reader_cpu_ns\":%s,\"reader_cycles\":%s,"
        "\"latency_p50_us\":%.3f,\"latency_p95_us\":%.3f,\"latency_max_us\":%.3f,\"valid\":%s}\n",
        hint == Hint::ADAPTIVE ? "adaptive" : "blocking", readers,
        static_cast<unsigned long long>(messages), elapsed, static_cast<unsigned long long>(received),
        cpu.c_str(), cycle_value.c_str(), percentile_us(0.50), percentile_us(0.95),
        percentile_us(1.0), valid ? "true" : "false");
    return valid;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (sintra::test::has_branch_flag(argc, argv)) {
            return managed_test(argc, argv);
        }
        if (sintra::test::has_argv_flag(argc, argv, "--benchmark")) {
            const auto seconds = std::stoul(sintra::test::get_argv_value(argc, argv, "--seconds", "10"));
            const auto readers = std::stoul(sintra::test::get_argv_value(argc, argv, "--readers", "8"));
            if (seconds < 1 || seconds > 120 || readers < 1 || readers > 64) {
                throw std::invalid_argument("benchmark requires seconds=1..120 and readers=1..64");
            }
            const bool adaptive = benchmark_policy(Hint::ADAPTIVE, readers, seconds);
            const bool blocking = benchmark_policy(Hint::BLOCKING, readers, seconds);
            return adaptive && blocking ? 0 : 1;
        }
        bool ok = default_and_pending_data();
        for (const auto action : {wake_action::publish, wake_action::stop,
                wake_action::unblock, wake_action::final_close})
        {
            ok = registered_wakes(action) && ok;
        }
        ok = batch_hint_lifetime() && ok;
        const int managed_result = managed_test(argc, argv);
        return ok && managed_result == 0 ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s%s\n", k_prefix, error.what());
        return 1;
    }
}
