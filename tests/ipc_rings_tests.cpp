#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Tune eviction behaviour for faster unit tests before including the ring header.
#define SINTRA_EVICTION_SPIN_THRESHOLD 0
#define SINTRA_RING_TEST_HOOKS

// Expose private members of the ring classes so tests can introspect internal
// state and simulate races. This is confined to the test translation unit.
#define private public
#define protected public
#include "sintra/detail/ipc_rings.h"
#undef private
#undef protected

using namespace std::chrono_literals;

#ifdef SINTRA_RING_TEST_HOOKS
namespace sintra::ring_detail {
void (*g_ring_r_guard_retry_hook)(void* reader) = nullptr;
void* g_ring_r_guard_retry_context               = nullptr;
}
#endif

namespace {

class Assertion_error : public std::runtime_error {
public:
    Assertion_error(const std::string& expr, const char* file, int line, const std::string& message = {})
        : std::runtime_error(make_message(expr, file, line, message)) {}

private:
    static std::string make_message(const std::string& expr, const char* file, int line, const std::string& message)
    {
        std::ostringstream oss;
        oss << file << ':' << line << " - assertion failed: " << expr;
        if (!message.empty()) {
            oss << " (" << message << ')';
        }
        return oss.str();
    }
};

#define ASSERT_TRUE(expr) do { if (!(expr)) throw Assertion_error(#expr, __FILE__, __LINE__); } while (false)
#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))
#define ASSERT_EQ(expected, actual) do { \
    auto _exp = (expected); \
    auto _act = (actual); \
    if (!(_exp == _act)) { \
        std::ostringstream _oss; \
        _oss << "expected " << _exp << ", got " << _act; \
        throw Assertion_error(#expected " == " #actual, __FILE__, __LINE__, _oss.str()); \
    } \
} while (false)
#define ASSERT_NE(val1, val2) do { \
    auto _v1 = (val1); \
    auto _v2 = (val2); \
    if (_v1 == _v2) { \
        std::ostringstream _oss; \
        _oss << "values both equal to " << _v1; \
        throw Assertion_error(#val1 " != " #val2, __FILE__, __LINE__, _oss.str()); \
    } \
} while (false)
#define ASSERT_GE(val, ref) ASSERT_TRUE((val) >= (ref))
#define ASSERT_LE(val, ref) ASSERT_TRUE((val) <= (ref))
#define ASSERT_GT(val, ref) ASSERT_TRUE((val) > (ref))
#define ASSERT_LT(val, ref) ASSERT_TRUE((val) < (ref))
#define ASSERT_THROW(statement, exception_type) do { \
    bool _thrown = false; \
    try { \
        statement; \
    } \
    catch (const exception_type&) { \
        _thrown = true; \
    } \
    catch (...) { \
    } \
    if (!_thrown) { throw Assertion_error("Expected exception " #exception_type, __FILE__, __LINE__); } \
} while (false)

struct Test_case {
    std::string name;
    std::function<void()> fn;
    bool is_stress = false;
};

inline std::vector<Test_case>& registry()
{
    static std::vector<Test_case> tests;
    return tests;
}

struct Register_test {
    Register_test(const std::string& name, std::function<void()> fn, bool is_stress)
    {
        registry().push_back({name, std::move(fn), is_stress});
    }
};

#define TEST_CASE(name) \
    void name(); \
    static Register_test name##_registrar(#name, name, false); \
    void name()

#define STRESS_TEST(name) \
    void name(); \
    static Register_test name##_registrar(#name, name, true); \
    void name()

std::atomic<uint64_t>& test_counter()
{
    static std::atomic<uint64_t> counter{0};
    return counter;
}

struct Temp_ring_dir {
    std::filesystem::path path;

    explicit Temp_ring_dir(const std::string& hint)
    {
        auto base = std::filesystem::temp_directory_path() / "sintra_ipc_ring_tests";
        std::filesystem::create_directories(base);

        // Simple unique directory name
        auto id = test_counter().fetch_add(1, std::memory_order_relaxed);
        path = base / (hint + '_' + std::to_string(id));
        std::filesystem::create_directories(path);
    }

    ~Temp_ring_dir()
    {
        // Clean removal - no delays needed!
        // The library now handles uniqueness internally, so rapid create/destroy
        // never tries to reuse the same files.
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        // Ignore errors - temp cleanup is best-effort
    }

    std::string str() const { return path.string(); }
};

#ifdef SINTRA_RING_TEST_HOOKS
struct Guard_retry_hook_context {
    std::atomic<bool>*                 eviction_triggered = nullptr;
    sintra::sequence_counter_type      advanced_sequence  = 0;
};

void guard_retry_hook_impl(void* reader_ptr)
{
    auto* ctx = static_cast<Guard_retry_hook_context*>(
        sintra::ring_detail::g_ring_r_guard_retry_context);
    if (!ctx || !ctx->eviction_triggered || ctx->eviction_triggered->exchange(true)) {
        return;
    }

    auto* reader = static_cast<sintra::Ring_R<int>*>(reader_ptr);
    auto& control = reader->c;
    auto& slot    = control.reading_sequences[reader->m_rs_index].data;

    auto observed_octile = slot.trailing_octile.load(std::memory_order_relaxed);
    control.leading_sequence.store(ctx->advanced_sequence, std::memory_order_release);

    uint8_t expected = 1;
    if (slot.has_guard.compare_exchange_strong(
            expected, uint8_t{0}, std::memory_order_acq_rel))
    {
        const uint64_t guard_mask = uint64_t(1) << (8 * observed_octile);
        control.read_access.fetch_sub(guard_mask, std::memory_order_acq_rel);
        slot.status.store(
            sintra::Ring<int, true>::READER_STATE_EVICTED, std::memory_order_release);
    }
}
#endif

template <typename T>
size_t pick_ring_elements(size_t min_elements = 8)
{
    size_t page_size = boost::interprocess::mapped_region::get_page_size();
    size_t ring_bytes = page_size;
    while (true) {
        if (ring_bytes % sizeof(T) == 0) {
            size_t elems = ring_bytes / sizeof(T);
            if (elems % 8 == 0 && elems >= min_elements) {
                return elems;
            }
        }
        ring_bytes += page_size;
        if (ring_bytes > page_size * 1024) {
            throw std::runtime_error("Unable to find suitable ring size");
        }
    }
}

TEST_CASE(test_get_ring_configurations_properties)
{
    constexpr size_t min_elements = 64;
    size_t page_size = boost::interprocess::mapped_region::get_page_size();
    auto configs = sintra::get_ring_configurations<uint32_t>(min_elements, page_size * 8, 6);
    ASSERT_FALSE(configs.empty());
    ASSERT_LE(configs.size(), 6u);

    size_t previous = 0;
    for (auto count : configs) {
        ASSERT_EQ(count % 8, 0u);
        ASSERT_GE(count, min_elements);
        ASSERT_TRUE(((count * sizeof(uint32_t)) % page_size) == 0);
        ASSERT_GT(count, previous);
        previous = count;
    }
}

TEST_CASE(test_mod_helpers)
{
    ASSERT_EQ(sintra::mod_pos_i64(-1, 8), static_cast<size_t>(7));
    ASSERT_EQ(sintra::mod_pos_i64(9, 8), static_cast<size_t>(1));
    ASSERT_EQ(sintra::mod_pos_i64(-17, 16), static_cast<size_t>(15));
    ASSERT_EQ(sintra::mod_u64(17, 8), static_cast<size_t>(1));
    ASSERT_EQ(sintra::mod_u64(64, 8), static_cast<size_t>(0));
}

TEST_CASE(test_directory_helpers)
{
    Temp_ring_dir tmp("dir_helpers");
    auto dir_to_create = tmp.path / "nested";
    auto file_path = dir_to_create / "placeholder";

    std::error_code ec;
    std::filesystem::remove_all(dir_to_create, ec);
    ASSERT_TRUE(sintra::check_or_create_directory(dir_to_create.string()));
    ASSERT_TRUE(std::filesystem::is_directory(dir_to_create));

    {
        std::ofstream file(file_path);
        file << "x";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(file_path));

    ASSERT_TRUE(sintra::check_or_create_directory(file_path.string()));
    ASSERT_TRUE(std::filesystem::is_directory(file_path));

    ASSERT_TRUE(sintra::remove_directory(dir_to_create.string()));
    ASSERT_FALSE(std::filesystem::exists(dir_to_create));
}

TEST_CASE(test_ring_write_read_single_reader)
{
    Temp_ring_dir tmp("single_reader");
    size_t ring_elements = pick_ring_elements<int>(128);

    sintra::Ring_W<int> writer(tmp.str(), "ring_data", ring_elements);
    sintra::Ring_R<int> reader(tmp.str(), "ring_data", ring_elements, (ring_elements * 3) / 4);

    std::vector<int> payload{0, 1, 2, 3, 4, 5, 6, 7};
    ASSERT_LE(payload.size(), ring_elements / 8);

    auto dest = writer.write(payload.data(), payload.size());
    dest[3] = 1337; // in-place modification before publishing
    payload[3] = 1337;
    writer.done_writing();

    auto range = reader.start_reading(payload.size());
    ASSERT_EQ(static_cast<size_t>(range.end - range.begin), payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        ASSERT_EQ(range.begin[i], payload[i]);
    }

    ASSERT_THROW(reader.start_reading(), std::logic_error);
    reader.done_reading();

    ASSERT_EQ(writer.get_leading_sequence(), payload.size());
}

TEST_CASE(test_multiple_readers_see_same_data)
{
    Temp_ring_dir tmp("multi_reader");
    size_t ring_elements = pick_ring_elements<int>(256);
    sintra::Ring_W<int> writer(tmp.str(), "ring_data", ring_elements);

    std::vector<std::unique_ptr<sintra::Ring_R<int>>> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back(std::make_unique<sintra::Ring_R<int>>(tmp.str(), "ring_data", ring_elements, (ring_elements * 3) / 4));
    }

    std::vector<int> payload(ring_elements / 16);
    std::iota(payload.begin(), payload.end(), 10);
    ASSERT_LE(payload.size(), ring_elements / 8);
    writer.write(payload.data(), payload.size());
    writer.done_writing();

    for (auto& reader : readers) {
        auto range = reader->start_reading(payload.size());
        ASSERT_EQ(static_cast<size_t>(range.end - range.begin), payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            ASSERT_EQ(range.begin[i], payload[i]);
        }
        reader->done_reading();
    }
}

TEST_CASE(test_snapshot_raii)
{
    Temp_ring_dir tmp("snapshot");
    size_t ring_elements = pick_ring_elements<int>(128);
    sintra::Ring_W<int> writer(tmp.str(), "ring_data", ring_elements);
    sintra::Ring_R<int> reader(tmp.str(), "ring_data", ring_elements, (ring_elements * 3) / 4);

    std::vector<int> payload{5, 6, 7, 8, 9, 10};
    writer.write(payload.data(), payload.size());
    writer.done_writing();

    {
        auto snapshot = sintra::make_snapshot(reader, payload.size());
        auto range = snapshot.range();
        ASSERT_EQ(static_cast<size_t>(range.end - range.begin), payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            ASSERT_EQ(range.begin[i], payload[i]);
        }
        // destructor will call done_reading()
    }

    auto second_snapshot = sintra::make_snapshot(reader, payload.size());
    second_snapshot.dismiss();
    reader.done_reading();
}

TEST_CASE(test_wait_for_new_data)
{
    Temp_ring_dir tmp("wait_for_new_data");
    size_t ring_elements = pick_ring_elements<int>(128);
    sintra::Ring_W<int> writer(tmp.str(), "ring_data", ring_elements);
    auto reader = std::make_shared<sintra::Ring_R<int>>(tmp.str(), "ring_data", ring_elements, (ring_elements * 3) / 4);

    std::vector<int> observed;
    std::atomic<bool> ready{false};
    std::exception_ptr thread_error;

    std::thread reader_thread([&]() {
        try {
            auto initial = reader->start_reading();
            ASSERT_EQ(static_cast<size_t>(initial.end - initial.begin), size_t(0));
            ready.store(true, std::memory_order_release);

            auto range = reader->wait_for_new_data();
            if (range.begin && range.begin != range.end) {
                observed.assign(range.begin, range.end);
                reader->done_reading_new_data();
            }
            reader->done_reading();
        }
        catch (...) {
            thread_error = std::current_exception();
        }
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
    }

    std::vector<int> payload{11, 12, 13, 14};
    writer.write(payload.data(), payload.size());
    writer.done_writing();
    writer.unblock_global();

    reader_thread.join();
    if (thread_error) {
        std::rethrow_exception(thread_error);
    }

    ASSERT_EQ(observed.size(), payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        ASSERT_EQ(observed[i], payload[i]);
    }
}

TEST_CASE(test_reader_eviction_does_not_underflow_octile_counter)
{
    Temp_ring_dir tmp("guard_underflow");
    const std::string ring_name = "ring_data";
    const size_t ring_elements = pick_ring_elements<uint32_t>(64);
    const size_t trailing_cap = (ring_elements * 3) / 4;

    sintra::Ring_W<uint32_t> writer(tmp.str(), ring_name, ring_elements);
    sintra::Ring_R<uint32_t> reader(tmp.str(), ring_name, ring_elements, trailing_cap);

    const size_t block_elements = ring_elements / 8;
    std::vector<uint32_t> block(block_elements);

    auto write_block = [&](uint32_t seed) {
        for (size_t i = 0; i < block_elements; ++i) {
            block[i] = seed + static_cast<uint32_t>(i);
        }
        writer.write(block.data(), block.size());
        writer.done_writing();
    };

    for (uint32_t i = 0; i < ring_elements * 2; ++i) {
        write_block(i);
    }

    const int slot_index = reader.m_rs_index;
    auto& slot = reader.c.reading_sequences[slot_index].data;

    std::atomic<bool> guard_ready{false};

    std::thread writer_thread([&]{
        while (!guard_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int iter = 0; iter < static_cast<int>(ring_elements * 4); ++iter) {
            write_block(1000u + static_cast<uint32_t>(iter));
        }
    });

    std::thread reader_thread([&]{
        reader.start_reading(trailing_cap);
    });

    uint8_t guarded_octile = 0;
    auto guard_deadline = std::chrono::steady_clock::now() + 1s;
    bool guard_observed = false;
    while (std::chrono::steady_clock::now() < guard_deadline) {
        if (slot.has_guard.load(std::memory_order_acquire)) {
            guarded_octile = slot.trailing_octile.load(std::memory_order_relaxed);
            guard_observed = true;
            break;
        }
        std::this_thread::yield();
    }
    ASSERT_TRUE(guard_observed);

    guard_ready.store(true, std::memory_order_release);

    auto eviction_deadline = std::chrono::steady_clock::now() + 2s;
    bool eviction_observed = false;
    while (std::chrono::steady_clock::now() < eviction_deadline) {
        auto status = slot.status.load(std::memory_order_acquire);
        if (status == sintra::Ring<uint32_t, true>::READER_STATE_EVICTED) {
            eviction_observed = true;
            break;
        }
        std::this_thread::yield();
    }
    ASSERT_TRUE(eviction_observed);

    reader_thread.join();

    uint64_t read_access = reader.c.read_access.load(std::memory_order_acquire);
    const uint64_t guard_mask = uint64_t(1) << (guarded_octile * 8);
    uint8_t guard_count = static_cast<uint8_t>((read_access >> (guarded_octile * 8)) & 0xffu);

    reader.c.read_access.fetch_add(guard_mask, std::memory_order_release);
    slot.status.store(sintra::Ring<uint32_t, true>::READER_STATE_ACTIVE, std::memory_order_release);

    writer_thread.join();

    reader.done_reading();

    ASSERT_EQ(0u, guard_count);
}

TEST_CASE(test_slow_reader_eviction_restores_status)
{
    Temp_ring_dir tmp("eviction_status");
    const size_t ring_elements = pick_ring_elements<uint64_t>(128);
    const size_t trailing_cap  = (ring_elements * 3) / 4;

    sintra::Ring_R<uint64_t> reader(tmp.str(), "ring_data", ring_elements, trailing_cap);

    auto& control = reader.c;
    auto& slot    = control.reading_sequences[reader.m_rs_index].data;

    const auto trailing_idx = sintra::mod_pos_i64(
        static_cast<int64_t>(reader.m_reading_sequence->load(std::memory_order_relaxed)) -
        static_cast<int64_t>(reader.m_max_trailing_elements),
        reader.m_num_elements);
    const auto trailing_octile = (8 * trailing_idx) / reader.m_num_elements;

    reader.m_trailing_octile = static_cast<uint8_t>(trailing_octile);
    slot.trailing_octile.store(static_cast<uint8_t>(trailing_octile), std::memory_order_relaxed);

    const uint64_t guard_mask = uint64_t(1) << (8 * trailing_octile);
    control.read_access.store(guard_mask, std::memory_order_relaxed);
    slot.has_guard.store(1, std::memory_order_relaxed);
    slot.status.store(sintra::Ring<uint64_t, true>::READER_STATE_ACTIVE, std::memory_order_relaxed);

    slot.has_guard.store(0, std::memory_order_relaxed);
    slot.status.store(sintra::Ring<uint64_t, true>::READER_STATE_EVICTED, std::memory_order_relaxed);
    control.read_access.fetch_sub(guard_mask, std::memory_order_relaxed);

    reader.done_reading_new_data();

    auto restored_status = slot.status.load(std::memory_order_acquire);
    const auto expected_status = sintra::Ring<uint64_t, true>::READER_STATE_ACTIVE;
    ASSERT_EQ(expected_status, restored_status);
}

TEST_CASE(test_streaming_reader_status_not_restored_after_eviction)
{
    Temp_ring_dir tmp("streaming_eviction_state");
    const size_t ring_elements = pick_ring_elements<uint32_t>(64);
    const size_t trailing_cap  = (ring_elements * 3) / 4;

    sintra::Ring_R<uint32_t> reader(tmp.str(), "ring_data", ring_elements, trailing_cap);
    auto& control = reader.c;
    auto& slot    = control.reading_sequences[reader.m_rs_index].data;

    const auto initial_leading =
        static_cast<sintra::sequence_counter_type>(trailing_cap + ring_elements / 8);
    const auto initial_reading = initial_leading - static_cast<sintra::sequence_counter_type>(ring_elements / 8);

    control.leading_sequence.store(initial_leading, std::memory_order_release);
    reader.m_reading_sequence->store(initial_reading, std::memory_order_release);
    slot.v.store(initial_reading, std::memory_order_release);
    control.read_access.store(0, std::memory_order_relaxed);
    slot.has_guard.store(0, std::memory_order_relaxed);
    slot.status.store(sintra::Ring<uint32_t, true>::READER_STATE_ACTIVE, std::memory_order_relaxed);

    auto first_range = reader.wait_for_new_data();
    ASSERT_TRUE(first_range.end >= first_range.begin);
    reader.done_reading_new_data();

    const uint8_t guarded_octile = slot.trailing_octile.load(std::memory_order_acquire);
    const uint64_t guard_mask     = uint64_t(1) << (8 * guarded_octile);

    uint8_t expected = 1;
    ASSERT_TRUE(slot.has_guard.compare_exchange_strong(
        expected, uint8_t{0}, std::memory_order_acq_rel));
    control.read_access.fetch_sub(guard_mask, std::memory_order_acq_rel);
    slot.status.store(sintra::Ring<uint32_t, true>::READER_STATE_EVICTED, std::memory_order_release);

    control.leading_sequence.fetch_add(ring_elements / 4, std::memory_order_release);
    reader.m_reading_sequence->fetch_sub(ring_elements / 4, std::memory_order_release);
    slot.v.fetch_sub(ring_elements / 4, std::memory_order_release);

    auto second_range = reader.wait_for_new_data();
    ASSERT_TRUE(second_range.end >= second_range.begin);
    reader.done_reading_new_data();

    const auto evicted_state = sintra::Ring<uint32_t, true>::READER_STATE_EVICTED;
    ASSERT_EQ(evicted_state, slot.status.load(std::memory_order_acquire));

    ASSERT_THROW(reader.start_reading(), sintra::ring_reader_evicted_exception);

    const auto active_state = sintra::Ring<uint32_t, true>::READER_STATE_ACTIVE;
    slot.status.store(active_state, std::memory_order_release);
    control.read_access.store(0, std::memory_order_release);
    slot.has_guard.store(0, std::memory_order_release);
}

STRESS_TEST(stress_multi_reader_throughput)
{
    Temp_ring_dir tmp("stress_multi");
    size_t ring_elements = pick_ring_elements<uint64_t>(512);
    const size_t max_trailing = (ring_elements * 3) / 4;
    const size_t reader_count = 3;
    const size_t chunk = std::max<size_t>(1, ring_elements / 16);
    const size_t total_messages = chunk * 64;

    sintra::Ring_W<uint64_t> writer(tmp.str(), "ring_data", ring_elements);

    std::atomic<bool> writer_done{false};
    std::vector<std::vector<uint64_t>> reader_results(reader_count);
    std::vector<std::exception_ptr> reader_errors(reader_count);
    std::vector<std::atomic<bool>> reader_ready(reader_count);
    for (auto& flag : reader_ready) { flag.store(false, std::memory_order_relaxed); }

    std::vector<std::thread> reader_threads;
    reader_threads.reserve(reader_count);
    for (size_t rid = 0; rid < reader_count; ++rid) {
        reader_threads.emplace_back([&, rid]() {
            try {
                sintra::Ring_R<uint64_t> reader(tmp.str(), "ring_data", ring_elements, max_trailing);
                auto initial = reader.start_reading();
                ASSERT_EQ(static_cast<size_t>(initial.end - initial.begin), size_t(0));
                reader_ready[rid].store(true, std::memory_order_release);

                while (!writer_done.load(std::memory_order_acquire) || reader_results[rid].size() < total_messages) {
                    auto range = reader.wait_for_new_data();
                    if (!range.begin || range.begin == range.end) {
                        if (writer_done.load(std::memory_order_acquire)) {
                            break;
                        }
                        continue;
                    }
                    size_t len = static_cast<size_t>(range.end - range.begin);
                    reader_results[rid].insert(reader_results[rid].end(), range.begin, range.begin + len);
                    reader.done_reading_new_data();
                }
                reader.done_reading();
            }
            catch (...) {
                reader_errors[rid] = std::current_exception();
                reader_ready[rid].store(true, std::memory_order_release);
            }
        });
    }

    for (size_t rid = 0; rid < reader_count; ++rid) {
        while (!reader_ready[rid].load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }

    std::exception_ptr writer_error;
    std::thread writer_thread([&]() {
        try {
            std::vector<uint64_t> buffer(chunk);
            uint64_t seq = 0;
            while (seq < total_messages) {
                size_t count = std::min(chunk, static_cast<size_t>(total_messages - seq));
                for (size_t i = 0; i < count; ++i) {
                    buffer[i] = seq + i;
                }
                writer.write(buffer.data(), count);
                writer.done_writing();
                seq += count;
            }
        }
        catch (...) {
            writer_error = std::current_exception();
        }
        writer_done.store(true, std::memory_order_release);
        writer.unblock_global();
    });

    writer_thread.join();
    for (size_t rid = 0; rid < reader_count; ++rid) {
        reader_threads[rid].join();
    }

    if (writer_error) {
        std::rethrow_exception(writer_error);
    }
    for (auto& err : reader_errors) {
        if (err) {
            std::rethrow_exception(err);
        }
    }

    for (auto& results : reader_results) {
        ASSERT_EQ(results.size(), total_messages);
        for (size_t i = 0; i < total_messages; ++i) {
            ASSERT_EQ(results[i], static_cast<uint64_t>(i));
        }
    }
}

TEST_CASE(test_eviction_guard_underflow_and_status_stickiness)
{
    Temp_ring_dir tmp("eviction_underflow");
    auto ring_elements = pick_ring_elements<int>(64);
    sintra::Ring_W<int> writer(tmp.str(), "ring_data", ring_elements);
    (void)writer;

    const size_t max_trailing = (ring_elements * 3) / 4;
    sintra::Ring_R<int> reader(tmp.str(), "ring_data", ring_elements, max_trailing);

    auto& control = reader.c;
    auto& slot    = control.reading_sequences[reader.m_rs_index].data;

    const sintra::sequence_counter_type base_sequence =
        static_cast<sintra::sequence_counter_type>(max_trailing + ring_elements / 8);

    control.leading_sequence.store(base_sequence, std::memory_order_release);
    reader.m_reading_sequence->store(base_sequence, std::memory_order_release);
    slot.v.store(base_sequence, std::memory_order_release);

    control.read_access.store(0, std::memory_order_relaxed);
    slot.has_guard.store(0, std::memory_order_relaxed);
    slot.status.store(sintra::Ring<int, true>::READER_STATE_ACTIVE, std::memory_order_relaxed);
    slot.trailing_octile.store(0, std::memory_order_relaxed);

#ifdef SINTRA_RING_TEST_HOOKS
    std::atomic<bool> eviction_triggered{false};
    Guard_retry_hook_context hook_ctx;
    hook_ctx.eviction_triggered = &eviction_triggered;
    hook_ctx.advanced_sequence =
        static_cast<sintra::sequence_counter_type>(base_sequence + ring_elements / 8);

    struct Hook_guard {
        Hook_guard()
            : previous_hook(sintra::ring_detail::g_ring_r_guard_retry_hook)
            , previous_context(sintra::ring_detail::g_ring_r_guard_retry_context)
        {}

        ~Hook_guard()
        {
            sintra::ring_detail::g_ring_r_guard_retry_hook   = previous_hook;
            sintra::ring_detail::g_ring_r_guard_retry_context = previous_context;
        }

        void (*previous_hook)(void* reader);
        void* previous_context;
    } hook_guard;

    sintra::ring_detail::g_ring_r_guard_retry_context = &hook_ctx;
    sintra::ring_detail::g_ring_r_guard_retry_hook    = &guard_retry_hook_impl;
    (void)hook_guard;
#endif

    bool threw_exception = false;
    try {
        auto snapshot = reader.start_reading(max_trailing);
        (void)snapshot;
        reader.done_reading();
    }
    catch (const boost::interprocess::interprocess_exception& ex) {
        threw_exception = true;
        std::cerr << "start_reading threw: " << ex.what() << '\n';
    }

    ASSERT_FALSE(threw_exception);

#ifdef SINTRA_RING_TEST_HOOKS
    ASSERT_TRUE(eviction_triggered.load());
#endif

    const uint64_t read_access_value = control.read_access.load(std::memory_order_acquire);
    ASSERT_EQ(read_access_value, 0u);

    const uint8_t final_status = slot.status.load(std::memory_order_acquire);
    ASSERT_EQ(final_status, (sintra::Ring<int, true>::READER_STATE_ACTIVE));
}

STRESS_TEST(stress_attach_detach_readers)
{
    Temp_ring_dir tmp("stress_attach_detach");
    size_t ring_elements = pick_ring_elements<int>(256);
    sintra::Ring_W<int> writer(tmp.str(), "ring_data", ring_elements);

    std::vector<int> payload(ring_elements / 16, 42);
    writer.write(payload.data(), payload.size());
    writer.done_writing();

    for (int i = 0; i < 64; ++i) {
        sintra::Ring_R<int> reader(tmp.str(), "ring_data", ring_elements, (ring_elements * 3) / 4);
        auto snapshot = sintra::make_snapshot(reader, payload.size());
        auto range = snapshot.range();
        ASSERT_EQ(static_cast<size_t>(range.end - range.begin), payload.size());
        for (auto ptr = range.begin; ptr != range.end; ++ptr) {
            ASSERT_EQ(*ptr, 42);
        }
    }
}

int run_tests(bool include_unit, bool include_stress)
{
    int failures = 0;
    size_t executed = 0;
    for (const auto& test : registry()) {
        if (!include_stress && test.is_stress) {
            continue;
        }
        if (!include_unit && !test.is_stress) {
            continue;
        }

        ++executed;
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const Assertion_error& ex) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " - " << ex.what() << '\n';
        }
        catch (const std::exception& ex) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " - unexpected exception: " << ex.what() << '\n';
        }
        catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " - unknown exception" << '\n';
        }
    }
    std::cout << "==== Summary ====" << '\n';
    std::cout << "Tests executed: " << executed << " / " << registry().size() << '\n';
    std::cout << "Failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    bool include_unit = true;
    bool include_stress = true;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--stress-only") {
            include_unit = false;
            include_stress = true;
        }
        else
        if (arg == "--skip-stress") {
            include_stress = false;
        }
        else
        if (arg == "--unit-only") {
            include_unit = true;
            include_stress = false;
        }
    }

    return run_tests(include_unit, include_stress);
}
