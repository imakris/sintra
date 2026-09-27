// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define private public
#define protected public
#include <sintra/sintra.h>
#undef protected
#undef private

#include "test_ring_utils.h"
#include "test_copying_mark_utils.h"

namespace {

namespace cm = sintra::test::copying_mark;
using Reader = sintra::Ring_R<uint32_t>;
using Writer = sintra::Ring_W<uint32_t>;
using State = Writer::Reader_state_union;

class Event
{
public:
    void signal()
    {
        std::lock_guard lock(m_mutex);
        m_signaled = true;
        m_condition.notify_all();
    }

    bool wait()
    {
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(lock, std::chrono::seconds(5), [&]() { return m_signaled; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_signaled = false;
};

class Hooks
{
public:
    explicit Hooks(std::function<void(std::string_view, const std::atomic<uint64_t>*)> action)
    :
        m_action(std::move(action))
    {
        s_current = this;
        sintra::detail::test_hooks::s_ring_guard_operation = &observe;
    }

    ~Hooks()
    {
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
        s_current = nullptr;
    }

private:
    static void observe(const char* stage, const std::atomic<uint64_t>* access, uint8_t)
    {
        s_current->m_action(stage, access);
    }

    std::function<void(std::string_view, const std::atomic<uint64_t>*)> m_action;
    inline static Hooks* s_current = nullptr;
};

void check_evicted_recovery(bool stop)
{
    sintra::test::Temp_ring_dir directory("evicted_request_admission");
    const auto size = sintra::test::pick_ring_elements<uint32_t>(64);
    const auto octile_size = size / 8;
    Writer writer(directory.str(), "ring", size);
    Reader reader(directory.str(), "ring", size);
    reader.start_reading();
    std::vector<uint32_t> data(octile_size, 23);
    for (unsigned octile = 0; octile < 7; ++octile) {
        writer.write_commit(data.data(), data.size());
    }
    auto& slot = reader.c.reading_sequences[reader.m_rs_index].data;
    const auto saved_sequence = reader.reading_sequence();
    const auto saved_consumed = reader.m_last_consumed_sequence;
    const auto saved_octile = reader.m_trailing_octile;
    const auto saved_counts = reader.c.read_access.load();
    Event eviction_won, release_writer, denied, allow_retry, recovered, local_lock_acquired;
    std::atomic<bool> first_denial{true};
    std::exception_ptr writer_error, reader_error;
    Hooks hooks([&](std::string_view stage, const auto* access) {
        if (stage == "release" && access == &writer.c.read_access) {
            eviction_won.signal();
            cm::require(release_writer.wait(), "writer paired decrement watchdog");
        }
        if (stage == "recovery_denied" && access == &reader.c.read_access && first_denial.exchange(false)) {
            denied.signal();
            cm::require(allow_retry.wait(), "recovery denial watchdog");
        }
    });
    std::thread writing([&]() {
        try { writer.write_commit(data.data(), data.size()); }
        catch (...) { writer_error = std::current_exception(); }
    });
    const bool evicted = eviction_won.wait();
    std::thread recovery([&]() {
        try { reader.handle_eviction_if_needed(); }
        catch (...) { reader_error = std::current_exception(); }
        recovered.signal();
    });
    const bool waiting = denied.wait();
    const auto blocked_state = slot.load_state();
    const bool unchanged = blocked_state.status() == Writer::READER_STATE_EVICTED &&
        blocked_state.request_pending() && !blocked_state.guard_present() &&
        reader.reading_sequence() == saved_sequence && reader.m_last_consumed_sequence == saved_consumed &&
        reader.m_trailing_octile == saved_octile && reader.c.read_access == saved_counts &&
        !reader.consume_eviction_notification();

    bool yielded = true;
    bool stopped_before_decrement = true;
    bool request_still_pending = true;
    if (stop) {
        std::thread stopper([&]() {
            {
                Reader::Local_read_lock local_lock(reader.m_reading_lock);
                request_still_pending = slot.load_state().request_pending();
                reader.request_stop();
            }
            local_lock_acquired.signal();
        });
        allow_retry.signal();
        yielded = local_lock_acquired.wait();
        stopped_before_decrement = recovered.wait();
        release_writer.signal();
        stopper.join();
    }
    else {
        allow_retry.signal();
        release_writer.signal();
    }
    writing.join();
    recovery.join();
    cm::require(evicted && waiting && unchanged,
        "EVICTED recovery under REQUEST must preserve its old range and the writer's paired count");
    cm::require(!writer_error && !reader_error && writer.get_diagnostics().reader_eviction_count == 1,
        "only the real writer may complete the single eviction");
    if (stop) {
        cm::require(yielded && stopped_before_decrement && request_still_pending,
            "recovery must release its local lock so stop can finish while REQUEST and writer release remain pending");
        reader.done_reading();
        cm::require(reader.c.read_access == 0 && !slot.load_state().guard_present() &&
            !slot.load_state().guard_pending() && !slot.load_state().request_pending() &&
            !reader.consume_eviction_notification(),
            "stopped recovery must tear down without acquiring a guard or inventing a recovery notification");
    }
    else {
        const auto resumed = slot.load_state();
        cm::require(resumed.status() == Writer::READER_STATE_ACTIVE && resumed.guard_present() &&
            !resumed.request_pending() && !resumed.guard_pending() &&
            reader.c.read_access == sintra::octile_mask(resumed.guard_octile()) &&
            reader.reading_sequence() >= 7 * octile_size && reader.reading_sequence() <= size,
            "completed eviction must resume with exactly one confirmed guard and a fresh range");
        cm::require(reader.consume_eviction_notification() && !reader.consume_eviction_notification() &&
            !reader.handle_eviction_if_needed() && !reader.consume_eviction_notification(),
            "a completed recovery must report its actual eviction exactly once");
        reader.done_reading();
    }
}

void check_pc_release_retry(bool stop)
{
    sintra::test::Temp_ring_dir directory("pc_release_request");
    const auto size = sintra::test::pick_ring_elements<uint32_t>(64);
    const auto octile_size = size / 8;
    Writer writer(directory.str(), "ring", size);
    Reader neighbor(directory.str(), "ring", size);
    Reader reader(directory.str(), "ring", size, octile_size);
    neighbor.start_reading();
    auto& slot = reader.c.reading_sequences[reader.m_rs_index].data;
    auto& neighbor_slot = neighbor.c.reading_sequences[neighbor.m_rs_index].data;
    const auto neighbor_word = neighbor_slot.word.load();
    const auto saved_sequence = reader.reading_sequence();
    const auto saved_consumed = reader.m_last_consumed_sequence;
    Event acquired, confirm, denied, allow_retry, completed, local_lock_acquired;
    std::atomic<bool> first_acquisition{true};
    std::atomic<bool> first_denial{true};
    std::exception_ptr error;
    sintra::Range<uint32_t> range;
    Hooks hooks([&](std::string_view stage, const auto* access) {
        if (access != &reader.c.read_access) {
            return;
        }
        if (stage == "acquired" && first_acquisition.exchange(false)) {
            acquired.signal();
            cm::require(confirm.wait(), "initial P-C confirmation watchdog");
        }
        if (stage == "guard_denied" && first_denial.exchange(false)) {
            denied.signal();
            cm::require(allow_retry.wait(), "P-C release denial watchdog");
        }
    });
    std::thread snapshot([&]() {
        try { range = reader.start_reading(octile_size); }
        catch (...) { error = std::current_exception(); }
        completed.signal();
    });
    const bool published_guard = acquired.wait();
    // Move the actual published head after guard publication, forcing the
    // confirming caller into release_guard rather than another acquisition.
    std::vector<uint32_t> data(octile_size, 71);
    writer.write_commit(data.data(), data.size());
    slot.word.fetch_or(State::request_mask);
    const auto blocked_word = slot.word.load();
    const auto blocked_counts = reader.c.read_access.load();
    confirm.signal();
    const bool waiting = denied.wait();
    const bool unchanged = slot.word == blocked_word && reader.c.read_access == blocked_counts &&
        reader.reading_sequence() == saved_sequence && reader.m_last_consumed_sequence == saved_consumed &&
        !reader.consume_eviction_notification();

    bool yielded = true;
    bool request_still_pending = true;
    if (stop) {
        std::thread stopper([&]() {
            {
                Reader::Local_read_lock local_lock(reader.m_reading_lock);
                request_still_pending = slot.load_state().request_pending();
                reader.request_stop();
            }
            local_lock_acquired.signal();
        });
        allow_retry.signal();
        yielded = local_lock_acquired.wait();
        stopper.join();
    }
    else {
        slot.word.fetch_and(~State::request_mask);
        allow_retry.signal();
    }
    const bool finished = completed.wait();
    if (!finished) {
        reader.request_stop();
    }
    snapshot.join();
    cm::require(published_guard && waiting && unchanged && finished && !error,
        "failed P-C confirmation must retry REQUEST release without changing the range or paired count");
    if (stop) {
        cm::require(yielded && request_still_pending && !range.begin && !range.end,
            "denied P-C release must yield its local lock to stop and return no unconfirmed range");
        reader.done_reading();
        cm::require(reader.c.read_access == sintra::octile_mask(0) &&
            !slot.load_state().guard_present() && !slot.load_state().guard_pending() &&
            !slot.load_state().request_pending(),
            "stopped P-C retry must release its own count and clear REQUEST while preserving the neighbor");
    }
    else {
        cm::require(range.begin && range.end - range.begin == octile_size &&
            std::all_of(range.begin, range.end, [](uint32_t value) { return value == 71; }) &&
            reader.reading_sequence() == octile_size && reader.m_last_consumed_sequence == octile_size &&
            reader.c.read_access == 2 * sintra::octile_mask(0),
            "cancelled REQUEST must finish the old guard release and return the newly confirmed snapshot");
        reader.done_reading();
    }
    cm::require(neighbor_slot.word == neighbor_word && !reader.consume_eviction_notification() &&
        writer.get_diagnostics().reader_eviction_count == 0 && reader.c.guard_accounting_mismatch_count == 0,
        "P-C retry or stop must preserve neighboring ownership and report no invented eviction");
    neighbor.done_reading();
}

} // namespace

int main()
{
    try {
        check_evicted_recovery(false);
        check_evicted_recovery(true);
        check_pc_release_retry(false);
        check_pc_release_retry(true);
        std::puts("PASS REQUEST recovery and P-C release retry, accounting, notification and stop");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "copying_mark_admission_test: %s\n", error.what());
        return 1;
    }
}
