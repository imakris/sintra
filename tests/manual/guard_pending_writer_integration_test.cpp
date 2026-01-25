// guard_pending_writer_integration_test.cpp
// Manual regression test for the pending-guard/writer interaction.
// This test exercises the writer loop under timing/scheduling assumptions,
// so it was moved from unit tests to avoid flaky CI failures.

// Include test utilities FIRST to ensure all standard library headers are
// included with proper access specifiers before the private/protected hack.
#include <test_environment.h>
#include <test_ring_utils.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

// Tune eviction behavior for faster manual runs before including the ring header.
#define SINTRA_EVICTION_SPIN_THRESHOLD 0
#define SINTRA_STALE_GUARD_DELAY_MS 25
#define private public
#define protected public
#include "sintra/detail/ipc/rings.h"
#undef private
#undef protected

namespace {

using sintra::test::Temp_ring_dir;
using sintra::test::pick_ring_elements;

int run_test()
{
    Temp_ring_dir tmp("stale_guard_pending_manual");
    const std::string ring_name = "ring_data";
    const size_t ring_elements = pick_ring_elements<uint32_t>(64);

    sintra::Ring_W<uint32_t> writer(tmp.str(), ring_name, ring_elements);
    sintra::Ring_R<uint32_t> reader(tmp.str(), ring_name, ring_elements, ring_elements / 2);

    const size_t head_index = ring_elements / 8;
    const uint8_t new_octile = sintra::octile_of_index(head_index, ring_elements);
    writer.m_octile = static_cast<uint8_t>((new_octile + 7) % 8);

    const uint64_t guard_mask = sintra::octile_mask(new_octile);

    auto& slot = reader.c.reading_sequences[reader.m_rs_index].data;
    using Reader_state_union = sintra::Ring<uint32_t, true>::Reader_state_union;

    bool pending_set = false;
    slot.fetch_update_state_if(
        [&](Reader_state_union current) -> std::optional<Reader_state_union>
        {
            return current.with_pending(new_octile);
        },
        pending_set);

    if (!pending_set) {
        std::fprintf(stderr, "FAILED: pending guard was not set\n");
        return 1;
    }

    // Simulate a reader starting guard acquisition with a pending update.
    reader.c.read_access.fetch_add(guard_mask);

    std::atomic<bool> writer_started{false};
    std::thread writer_thread([&] {
        writer_started.store(true, std::memory_order_release);
        writer.advance_writer_octile_if_needed(head_index);
    });

    while (!writer_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(SINTRA_STALE_GUARD_DELAY_MS) * 2);

    if ((reader.c.read_access.load() & guard_mask) != guard_mask) {
        std::fprintf(stderr, "FAILED: read_access guard mask cleared too early\n");
        if (writer_thread.joinable()) {
            writer_thread.join();
        }
        return 1;
    }

    slot.set_guard_token(static_cast<uint8_t>(0x08 | new_octile));
    slot.clear_pending();

    reader.m_trailing_octile = new_octile;
    reader.m_reading = true;
    reader.m_reading_lock = false;
    reader.done_reading();

    if (writer_thread.joinable()) {
        writer_thread.join();
    }

    uint64_t read_access = reader.c.read_access.load();
    uint8_t guard_count = static_cast<uint8_t>((read_access >> (8 * new_octile)) & 0xffu);

    if (guard_count != 0) {
        std::fprintf(stderr, "FAILED: guard_count=%u after cleanup\n",
                     static_cast<unsigned>(guard_count));
        return 1;
    }

    if (writer.m_octile != new_octile) {
        std::fprintf(stderr, "FAILED: writer did not advance to expected octile\n");
        return 1;
    }

    std::fprintf(stderr, "PASS: pending guard prevented stale writer rollback\n");
    return 0;
}

} // namespace

int main()
{
    try {
        return run_test();
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "FAILED: exception: %s\n", ex.what());
        return 1;
    }
}
