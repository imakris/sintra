#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
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

namespace {

using namespace std::chrono_literals;
using Reader = sintra::Message_ring_R;
using Writer = sintra::Message_ring_W;
using Raw_reader = sintra::Ring_R<uint32_t>;
using Raw_writer = sintra::Ring_W<uint32_t>;
using Slot_word = sintra::Ring<char, true>::Reader_state_union;

struct payload_t
{
    uint64_t sequence;
    sintra::message_string text;
};

using Frame = sintra::Message<payload_t, void, 0x71A4E05ull>;

class Event
{
public:
    void signal()
    {
        std::lock_guard lock(m_mutex);
        m_signaled = true;
        m_condition.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout = 5000ms)
    {
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(lock, timeout, [&]() { return m_signaled; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_signaled = false;
};

void require(bool condition, const char* message)
{
    sintra::test::require_true(condition, "copying_mark_test: ", message);
}

class Hooks
{
public:
    explicit Hooks(std::function<void(std::string_view, const std::atomic<uint64_t>*, uint8_t)> action)
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
    static void observe(const char* stage, const std::atomic<uint64_t>* access, uint8_t octile)
    {
        s_current->m_action(stage, access, octile);
    }

    std::function<void(std::string_view, const std::atomic<uint64_t>*, uint8_t)> m_action;
    inline static Hooks* s_current = nullptr;
};

void write_frame(Writer& writer, uint64_t sequence, const std::string& text)
{
    writer.write<Frame>(sintra::vb_size<Frame>(sequence, text), sequence, text);
    writer.done_writing();
}

void check_independent_copy()
{
    sintra::test::Temp_ring_dir directory("copy_independent");
    Writer writer(directory.str(), "req", 1);
    Reader first(directory.str(), "req", 1);
    Reader second(directory.str(), "req", 1);
    first.start_reading();
    second.start_reading();
    const std::string original(8192, 'o');
    write_frame(writer, 1, original);
    Event entered, release, other_done, requested;
    const Frame* held = nullptr;
    const Frame* other = nullptr;
    std::exception_ptr first_error, second_error, writer_error;
    Hooks hooks([&](std::string_view stage, const auto* access, uint8_t) {
        if (stage == "copy_validated" && access == &first.c.read_access) {
            entered.signal();
            if (!release.wait()) {
                throw std::runtime_error("copy release watchdog");
            }
        }
        if (stage == "request_published") {
            requested.signal();
        }
    });
    std::thread copy([&]() {
        try { held = static_cast<Frame*>(first.fetch_message()); }
        catch (...) { first_error = std::current_exception(); }
    });
    const bool copying = entered.wait();
    std::thread other_copy([&]() {
        try { other = static_cast<Frame*>(second.fetch_message()); }
        catch (...) { second_error = std::current_exception(); }
        other_done.signal();
    });
    const bool independent = other_done.wait(500ms);
    if (!copying || !independent) {
        release.signal();
        copy.join();
        other_copy.join();
        require(copying, "first frame must reach the validated-copy interval");
        require(independent, "another reader must copy while the first reader is paused");
    }
    other_copy.join();
    second.done_reading();
    auto& slot = first.c.reading_sequences[first.m_rs_index].data;
    const auto protected_count = first.c.read_access.load();
    const bool marked = slot.load_state().copying();
    std::thread producer([&]() {
        try {
            const auto start = writer.get_leading_sequence();
            const std::string filler(32768, 'f');
            while (writer.get_leading_sequence() - start < 2 * sintra::message_ring_size) {
                write_frame(writer, 2, filler);
            }
        }
        catch (...) { writer_error = std::current_exception(); }
    });
    const bool arbitration = requested.wait();
    const bool protected_reader = slot.load_state().copying() && slot.load_state().guard_present() &&
        slot.status() == Writer::READER_STATE_ACTIVE && first.c.read_access == protected_count &&
        writer.get_diagnostics().reader_eviction_count == 0;
    release.signal();
    copy.join();
    producer.join();
    require(marked, "paused validation must retain COPYING");
    require(arbitration, "eligible writer must publish REQUEST while a copy is paused");
    require(protected_reader, "REQUEST must preserve the marked guard and its count");
    require(!writer_error && writer.get_diagnostics().reader_eviction_count == 1,
        "writer alone must evict once after the admitted copy clears");
    require(!first_error && !second_error && held && other, "both frame copies must succeed");
    require(held->sequence == 1 && static_cast<std::string>(held->text) == original,
        "one owned frame must remain intact after writer reuse");
    first.done_reading();
}

void check_request_denial_and_teardown()
{
    sintra::test::Temp_ring_dir directory("copy_request");
    Writer writer(directory.str(), "req", 1);
    Reader reader(directory.str(), "req", 1);
    reader.start_reading();
    write_frame(writer, 1, "first");
    write_frame(writer, 2, "second");
    require(reader.fetch_message() != nullptr, "first frame must be delivered");
    auto& slot = reader.c.reading_sequences[reader.m_rs_index].data;
    const auto sequence = reader.get_message_reading_sequence();
    const auto counts = reader.c.read_access.load();
    slot.word.fetch_or(Slot_word::request_mask);
    Event denied, finished;
    Frame* frame = nullptr;
    std::exception_ptr error;
    Hooks hooks([&](std::string_view stage, const auto* access, uint8_t) {
        if (stage == "copy_denied" && access == &reader.c.read_access) {
            denied.signal();
        }
    });
    std::thread fetcher([&]() {
        try { frame = static_cast<Frame*>(reader.fetch_message()); }
        catch (...) { error = std::current_exception(); }
        finished.signal();
    });
    const bool saw_denial = denied.wait();
    const bool unchanged = reader.get_message_reading_sequence() == sequence &&
        !reader.consume_eviction_notification() && reader.c.read_access == counts;
    slot.word.fetch_and(~Slot_word::request_mask);
    const bool resumed = finished.wait();
    if (!resumed) {
        reader.request_stop();
    }
    fetcher.join();
    require(saw_denial && unchanged, "REQUEST denial must preserve the frame range and loss/count state");
    require(resumed && !error && frame && frame->sequence == 2,
        "request cancellation must retry the same unread frame");

    slot.word.fetch_or(Slot_word::request_mask);
    reader.done_reading();
    const auto released = slot.load_state();
    require(!released.guard_present() && !released.guard_pending() && !released.request_pending() &&
        reader.c.read_access == 0 && !reader.consume_eviction_notification(),
        "ordinary snapshot release must resolve orphan REQUEST with one own-count decrement");
    require(!reader.is_stopping(), "ordinary release must permit snapshot reuse");
    reader.start_reading();
    write_frame(writer, 3, "reused");
    require(static_cast<Frame*>(reader.fetch_message())->sequence == 3,
        "a released reader must support another streaming session");
    reader.done_reading();
}

void check_continuous_copy_arbitration()
{
    sintra::test::Temp_ring_dir directory("continuous_copy");
    Writer writer(directory.str(), "req", 1);
    Reader reader(directory.str(), "req", 1);
    reader.start_reading();
    write_frame(writer, 1, "admitted");
    write_frame(writer, 2, "denied until writer resolves request");
    Event entered, release_copy, requested, release_writer, denied;
    std::atomic<unsigned> admitted{0};
    std::atomic<unsigned> delivered{0};
    std::atomic<bool> stop{false};
    std::exception_ptr read_error, write_error;
    Hooks hooks([&](std::string_view stage, const auto* access, uint8_t) {
        if (stage == "request_published") {
            requested.signal();
            require(release_writer.wait(), "writer arbitration watchdog");
        }
        if (access != &reader.c.read_access) {
            return;
        }
        if (stage == "copy_enter") {
            ++admitted;
        }
        if (stage == "copy_validated" && admitted == 1) {
            entered.signal();
            require(release_copy.wait(), "first continuous copy watchdog");
        }
        if (stage == "copy_denied") {
            denied.signal();
        }
    });
    std::thread consumer([&]() {
        try {
            while (!stop) {
                if (!reader.fetch_message()) {
                    break;
                }
                ++delivered;
            }
        }
        catch (...) { read_error = std::current_exception(); }
    });
    const bool first_entered = entered.wait();
    std::thread producer([&]() {
        try {
            const auto start = writer.get_leading_sequence();
            const std::string filler(32768, 'f');
            while (writer.get_leading_sequence() - start < sintra::message_ring_size) {
                write_frame(writer, 3, filler);
            }
        }
        catch (...) { write_error = std::current_exception(); }
    });
    const bool writer_requested = requested.wait();
    release_copy.signal();
    const bool reader_denied = denied.wait();
    const bool bounded = admitted == 1 && delivered == 1 && !reader.consume_eviction_notification();
    release_writer.signal();
    producer.join();
    stop = true;
    reader.request_stop();
    consumer.join();
    require(first_entered && writer_requested && reader_denied && bounded,
        "a continuous reader must finish at most its admitted copy and admit none after REQUEST");
    require(!read_error && !write_error && writer.get_diagnostics().reader_eviction_count == 1,
        "writer arbitration must complete without repeated reader admissions starving it");
    reader.done_reading();
}

void check_guard_admission()
{
    for (unsigned operation = 0; operation < 3; ++operation) {
        sintra::test::Temp_ring_dir directory("guard_request");
        const auto size = sintra::test::pick_ring_elements<uint32_t>(64);
        Raw_writer writer(directory.str(), "ring", size);
        Raw_reader reader(directory.str(), "ring", size, 0);
        auto& slot = reader.c.reading_sequences[reader.m_rs_index].data;
        if (operation == 1) {
            reader.start_reading();
            reader.m_reading_sequence->store(size / 8);
        }
        slot.word.fetch_or(Slot_word::request_mask);
        const auto before = reader.c.read_access.load();
        Event denied, completed;
        Hooks hooks([&](std::string_view stage, const auto* access, uint8_t) {
            if (stage == "guard_denied" && access == &reader.c.read_access) {
                denied.signal();
            }
        });
        std::exception_ptr error;
        std::thread transition([&]() {
            try {
                if (operation == 0) {
                    reader.start_reading();
                }
                else
                if (operation == 1) {
                    reader.done_reading_new_data();
                }
                else {
                    reader.reattach_after_eviction();
                }
            }
            catch (...) { error = std::current_exception(); }
            completed.signal();
        });
        const bool gated = denied.wait();
        const bool unchanged = reader.c.read_access == before && !reader.consume_eviction_notification();
        slot.word.fetch_and(~Slot_word::request_mask);
        const bool resumed = completed.wait();
        if (!resumed) {
            reader.request_stop();
        }
        transition.join();
        require(gated && unchanged && resumed && !error,
            "snapshot, guard move and direct reattach must retry REQUEST without invented eviction");
        if (operation == 2) {
            reader.m_reading = true;
        }
        reader.done_reading();
    }
}

void check_pending_completion()
{
    sintra::test::Temp_ring_dir directory("pending_request");
    const auto size = sintra::test::pick_ring_elements<uint32_t>(64);
    Raw_writer writer(directory.str(), "ring", size);
    Raw_reader reader(directory.str(), "ring", size, 0);
    reader.start_reading();
    const auto old_octile = reader.m_trailing_octile;
    reader.m_reading_sequence->store(size / 8);
    auto& slot = reader.c.reading_sequences[reader.m_rs_index].data;
    Event pending, release;
    Hooks hooks([&](std::string_view stage, const auto* access, uint8_t) {
        if (stage == "pending" && access == &reader.c.read_access) {
            pending.signal();
            require(release.wait(), "pending release watchdog");
        }
    });
    std::exception_ptr error;
    std::thread mover([&]() {
        try { reader.done_reading_new_data(); }
        catch (...) { error = std::current_exception(); }
    });
    const bool entered = pending.wait();
    slot.word.fetch_or(Slot_word::request_mask);
    release.signal();
    mover.join();
    const auto moved = slot.load_state();
    require(entered && !error && moved.guard_present() && moved.guard_octile() != old_octile &&
        !moved.guard_pending() && moved.request_pending(),
        "bookkeeping admitted before REQUEST must finish and retain writer arbitration");
    require(reader.c.read_access == sintra::octile_mask(moved.guard_octile()),
        "guard move must release exactly its old count");
    reader.done_reading();
}

void check_exception_clearing()
{
    for (const bool malformed : {true, false}) {
        sintra::test::Temp_ring_dir directory("copy_exception");
        Writer writer(directory.str(), "req", 1);
        Reader reader(directory.str(), "req", 1);
        reader.start_reading();
        write_frame(writer, 1, "checked");
        if (malformed) {
            const uint64_t invalid_magic = 0;
            std::memcpy(writer.m_data, &invalid_magic, sizeof(invalid_magic));
        }
        Hooks hooks([&](std::string_view stage, const auto*, uint8_t) {
            if (!malformed && stage == "copy_validated") {
                throw std::runtime_error("injected copy exception");
            }
        });
        bool failed = false;
        try { reader.fetch_message(); }
        catch (const std::exception&) { failed = true; }
        const auto state = reader.c.reading_sequences[reader.m_rs_index].data.load_state();
        require(failed && !state.copying() && state.guard_present(),
            "malformed frames and exceptional copy exits must clear only COPYING");
        reader.done_reading();
    }
}

} // namespace

int main()
{
    try {
        check_independent_copy();
        check_request_denial_and_teardown();
        check_continuous_copy_arbitration();
        check_guard_admission();
        check_pending_completion();
        check_exception_clearing();
        std::puts("PASS copying mark exclusion, arbitration, accounting and reusable release");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "copying_mark_test: %s\n", error.what());
        return 1;
    }
}
