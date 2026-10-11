// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

#define SINTRA_EVICTION_SPIN_THRESHOLD 0
#define SINTRA_EVICTION_SPIN_BUDGET_US 100
#define private public
#define protected public
#include <sintra/detail/messaging/message.h>
#undef protected
#undef private

#include "test_ring_utils.h"
#include "test_copying_mark_utils.h"
#include "test_copying_mark_death.h"
#include "test_process_identity_fakes.h"

namespace {

namespace cm = sintra::test::copying_mark;
namespace fakes = sintra::test::identity_fakes;
using Writer = sintra::Ring_W<uint32_t>;
using Reader = sintra::Ring_R<uint32_t>;
using State = Writer::Reader_state_union;

std::string executable;
std::filesystem::path child_directory;
const char* child_pause_stage = nullptr;
std::atomic<bool> writer_waiting{false};

void observe(const char* stage, const std::atomic<uint64_t>*, uint8_t octile)
{
    if (std::strcmp(stage, "writer_waiting") == 0) {
        writer_waiting = true;
    }
    if (child_pause_stage && std::strcmp(stage, child_pause_stage) == 0) {
        cm::signal_file(child_directory / "paused", std::to_string(octile));
        cm::wait_for_file(child_directory / "release");
    }
}

int run_acquisition_child(const std::string& directory)
{
    child_directory = directory;
    sintra::detail::test_hooks::s_ring_guard_operation = observe;
    child_pause_stage = "slot_acquired";
    Reader reader(directory, "raw", sintra::test::pick_ring_elements<uint32_t>());
    return 0;
}

void acquisition_death_restores_capacity()
{
    sintra::test::Temp_ring_dir directory("reader_acquisition_death");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader neighbor(directory.str(), "raw", elements);
    neighbor.start_reading();
    auto& control = writer.c;
    const auto count_before = control.read_access.load();
    cm::Test_child child(executable, {"--child-acquire", directory.str()});
    cm::wait_for_file(directory.path / "paused");
    child.terminate(0);
    cm::require(sintra::test::checked_scavenge_orphans(control), "INACTIVE popped slot must report recovered capacity");
    cm::require(control.read_access == count_before, "inactive cleanup must preserve neighbor's owned count");
    cm::require(control.free_rs_stack.size() == sintra::max_process_index - 1,
        "death between pop and ACTIVE must restore the missing free entry");
    cm::require(!sintra::test::checked_scavenge_orphans(control), "repeated inactive cleanup must not duplicate free entries");
    std::vector<std::unique_ptr<Reader>> readers;
    std::set<int> indices{neighbor.m_rs_index};
    for (int index = 1; index < sintra::max_process_index; ++index) {
        readers.push_back(std::make_unique<Reader>(directory.str(), "raw", elements));
        cm::require(indices.insert(readers.back()->m_rs_index).second, "reader freelist contains a duplicate");
    }
    cm::require(control.free_rs_stack.empty() && indices.size() == sintra::max_process_index,
        "all reader slots must be reacquirable after acquisition death");
    neighbor.done_reading();
}

void fill_to(Writer& writer, uint64_t target)
{
    std::vector<uint32_t> values(writer.m_num_elements / 8, 17);
    while (writer.get_leading_sequence() < target) {
        const size_t amount = std::min<uint64_t>(values.size(), target - writer.get_leading_sequence());
        writer.write(values.data(), amount);
        writer.done_writing();
    }
}

sintra::process_identity_result_t unknown_identity(const sintra::process_incarnation_t&)
{
#ifdef _WIN32
    constexpr int error = ERROR_ACCESS_DENIED;
#else
    constexpr int error = EACCES;
#endif
    return {sintra::Process_identity_status::UNKNOWN, std::error_code(error, std::system_category())};
}

// A live marked reader whose native observation cannot prove its death. The
// injection may publish an identity whose process record shows another
// incarnation, so that only the observation failure protects the reader.
struct protection_case_t
{
    const char* name;
    int         expected_error;
    void      (*inject)(sintra::process_incarnation_t& published);
};

#ifdef _WIN32
sintra::process_identity_result_t denied_identity(const sintra::process_incarnation_t&)
{
    return {sintra::Process_identity_status::UNKNOWN,
        std::error_code(ERROR_ACCESS_DENIED, std::system_category())};
}

const protection_case_t k_protection_cases[] = {
    {"access_denied", ERROR_ACCESS_DENIED, [](sintra::process_incarnation_t&) {
        sintra::detail::process_identity_probe_hook = denied_identity;
    }},
};
#elif defined(__linux__)
void publish_foreign_record(sintra::process_incarnation_t& published)
{
    published.start_stamp += 1;
    published.namespaces = {fakes::valid_namespace(0x11), fakes::valid_namespace(0x21)};
}

const protection_case_t k_protection_cases[] = {
    {"hidden_record_live_pidfd", ENOENT, [](sintra::process_incarnation_t&) {
        fakes::s_hide_process_records = true;
        fakes::s_pidfd_result = fakes::k_succeed;
    }},
    {"hidden_record_live_signal", ENOENT, [](sintra::process_incarnation_t&) {
        fakes::s_hide_process_records = true;
        fakes::s_pidfd_result = ENOSYS;
    }},
    {"hidden_record_denied_signal", EPERM, [](sintra::process_incarnation_t&) {
        fakes::s_hide_process_records = true;
        fakes::s_pidfd_result = ENOSYS;
        fakes::s_kill_result = EPERM;
    }},
    {"foreign_pid_namespace", sintra::detail::k_foreign_process_record_error,
        [](sintra::process_incarnation_t& published) {
            publish_foreign_record(published);
            fakes::s_namespaces = sintra::process_namespaces_t{
                fakes::valid_namespace(0x12), fakes::valid_namespace(0x21)};
        }},
    {"foreign_time_namespace", sintra::detail::k_foreign_process_record_error,
        [](sintra::process_incarnation_t& published) {
            publish_foreign_record(published);
            fakes::s_namespaces = sintra::process_namespaces_t{
                fakes::valid_namespace(0x11), fakes::valid_namespace(0x22)};
        }},
    {"ancestor_procfs_view", sintra::detail::k_foreign_process_record_error,
        [](sintra::process_incarnation_t& published) {
            publish_foreign_record(published);
            fakes::s_namespaces = published.namespaces;
            fakes::use_status_nstgid("4321\t17");
        }},
};
#elif defined(__APPLE__)
const protection_case_t k_protection_cases[] = {
    {"missing_record_live_signal", ESRCH, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::FAILED;
    }},
    {"missing_record_denied_signal", EPERM, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::FAILED;
        fakes::s_kill_result = EPERM;
    }},
    {"record_denied", EPERM, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::FAILED;
        fakes::s_record_error = EPERM;
    }},
    {"malformed_record", EIO, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::SHORT;
    }},
};
#elif defined(__FreeBSD__)
const protection_case_t k_protection_cases[] = {
    {"missing_record_live_signal", ESRCH, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::FAILED;
    }},
    {"empty_record_live_signal", ESRCH, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::EMPTY;
    }},
    {"missing_record_capability_mode_signal", ECAPMODE, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::FAILED;
        fakes::s_kill_result = ECAPMODE;
    }},
    {"missing_record_denied_signal", EPERM, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::FAILED;
        fakes::s_kill_result = EPERM;
    }},
    {"record_capability_mode", ECAPMODE, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::FAILED;
        fakes::s_record_error = ECAPMODE;
    }},
    {"short_record", EIO, [](sintra::process_incarnation_t&) {
        fakes::s_record = fakes::Record::SHORT;
    }},
    {"unstable_boot_time", EAGAIN, [](sintra::process_incarnation_t& published) {
        published.start_stamp += 1000;
        fakes::s_moving_boot_time_lookups = fakes::k_every_lookup;
    }},
    // A boot-time change reversed between the lookups around the observed
    // record, or around the captured one, which stable observations then find
    // to differ.
    {"boot_time_change_during_observation", sintra::detail::k_start_stamp_mismatch_error,
        [](sintra::process_incarnation_t&) {
            fakes::s_record_boot_time_shift = 3600;
        }},
    {"boot_time_change_during_capture", sintra::detail::k_start_stamp_mismatch_error,
        [](sintra::process_incarnation_t& published) {
            published.start_stamp += 3600ull * 1'000'000'000;
        }},
};
#endif

bool same_incarnation(const sintra::process_incarnation_t& a, const sintra::process_incarnation_t& b)
{
    const auto same_namespace = [](const sintra::process_namespace_t& x, const sintra::process_namespace_t& y) {
        return x.state == y.state && x.device == y.device && x.inode == y.inode;
    };
    return a.pid == b.pid && a.start_stamp == b.start_stamp &&
        same_namespace(a.namespaces.pid, b.namespaces.pid) &&
        same_namespace(a.namespaces.time, b.namespaces.time);
}

class Held_copy
{
public:
    explicit Held_copy(Reader& reader) : m_reader(reader), m_thread([this]() {
        Reader::Local_read_lock local_lock(m_reader.m_reading_lock);
        m_admission = m_reader.with_copying_mark([&]() {
            m_entered = true;
            while (!m_release) {
                std::this_thread::yield();
            }
            std::memcpy(&m_owned, m_reader.get_base_address(), sizeof(m_owned));
        });
    })
    {
        cm::require(cm::wait_until([&]() { return m_entered.load(); }), "copy admission did not complete");
    }

    ~Held_copy() { release(); }

    void release()
    {
        m_release = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    Reader& m_reader;
    std::atomic<bool> m_entered{false};
    std::atomic<bool> m_release{false};
    uint32_t m_owned = 0;
    Reader::Copy_admission m_admission = Reader::Copy_admission::STOPPED;
    std::thread m_thread;
};

void live_owner_protection(const protection_case_t& protection)
{
    sintra::test::Temp_ring_dir directory("live_owner_protection");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader reader(directory.str(), "raw", elements);
    reader.start_reading();
    auto& slot = writer.c.reading_sequences[reader.m_rs_index].data;
    const auto guarded_octile = slot.load_state().guard_octile();
    const auto target = guarded_octile == 0 ? elements : uint64_t(guarded_octile) * (elements / 8);
    fill_to(writer, target - 1);

    Held_copy copy(reader);
    const auto own_identity = slot.owner();
    auto published = own_identity;
    const auto protected_word = slot.word.load();
    const auto protected_count = writer.c.read_access.load();
    cm::require(State{protected_word}.copying() && State{protected_word}.guard_present(),
        "protection case must hold a real copying mark and its guard");

    sintra::process_identity_result_t observed{};
    bool blocked = false;
    {
        fakes::Scoped_fakes injected;
        protection.inject(published);
        slot.publish_owner(published);
        observed = sintra::probe_process_identity(published);
        try {
            const uint32_t value = 45678;
            writer.write(&value, 1);
        }
        catch (const std::system_error& error) {
            blocked = error.code() == observed.error;
        }
        sintra::detail::process_identity_probe_hook = nullptr;
    }

    cm::require(observed.status == sintra::Process_identity_status::UNKNOWN &&
        observed.error.value() == protection.expected_error && blocked,
        "an unproven death must remain UNKNOWN and stop a blocked writer with its native error");
    cm::require(slot.word == protected_word && writer.c.read_access == protected_count &&
        same_incarnation(slot.owner(), published),
        "an unproven death must preserve COPYING, guard, count and exact owner identity");
    cm::require(writer.c.reader_eviction_count == 0 && !reader.consume_eviction_notification(),
        "an unproven death must not invent an eviction or lost range");
    slot.publish_owner(own_identity);
    copy.release();
    reader.done_reading();
}

void unknown_reservation_rollback(bool inherited)
{
    sintra::test::Temp_ring_dir directory("unknown_reservation");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader reader(directory.str(), "raw", elements);
    reader.start_reading();
    auto& slot = writer.c.reading_sequences[reader.m_rs_index].data;
    const auto guarded_octile = slot.load_state().guard_octile();
    const auto target = guarded_octile == 0 ? elements : uint64_t(guarded_octile) * (elements / 8);
    fill_to(writer, target - (inherited ? 2 : 1));
    uint32_t value_a = 12345;
    uint32_t* original = inherited ? writer.write(&value_a, 1) : nullptr;
    const auto saved_sequence = writer.m_pending_new_sequence;
    const auto saved_octile = writer.m_octile;
    const auto saved_owner = writer.m_writing_thread_index.load();
    const auto saved_head = writer.get_leading_sequence();
    const auto saved_count = writer.c.read_access.load();
    Held_copy copy(reader);
    const auto protected_word = slot.word.load();
    const uint32_t before_failed_destination = writer.m_data[saved_sequence % elements];
    sintra::detail::process_identity_probe_hook = unknown_identity;
    bool failed_natively = false;
    try {
        const uint32_t rejected_value = 98765;
        writer.write(&rejected_value, 1);
    }
    catch (const std::system_error& error) {
        failed_natively = error.code() == unknown_identity({}).error;
    }
    sintra::detail::process_identity_probe_hook = nullptr;
    cm::require(failed_natively, "blocking UNKNOWN must return its bounded native writer error");
    cm::require(writer.m_pending_new_sequence == saved_sequence && writer.m_octile == saved_octile &&
        writer.m_writing_thread_index == saved_owner && writer.get_leading_sequence() == saved_head,
        "failed reservation must restore exact sequence/octile and ownership provenance");
    cm::require(slot.word == protected_word && writer.c.read_access == saved_count &&
        writer.m_data[saved_sequence % elements] == before_failed_destination,
        "UNKNOWN must preserve guard/count/COPYING and leave the rejected payload unwritten");

    if (inherited) {
        writer_waiting = false;
        sintra::detail::test_hooks::s_ring_guard_operation = observe;
        std::atomic<bool> second_finished{false};
        std::thread second([&]() {
            writer.write_commit(uint32_t{24680});
            second_finished = true;
        });
        const bool observed_wait = cm::wait_until([&]() { return writer_waiting.load(); });
        const bool excluded = writer.m_writing_thread_index == saved_owner && !second_finished;
        *original = 54321;
        copy.release();
        reader.done_reading();
        const auto published_a = writer.done_writing();
        second.join();
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
        cm::require(observed_wait && excluded, "T2 must wait for the inherited reservation owned by T1");
        cm::require(published_a == saved_sequence && *original == 54321 &&
            writer.get_leading_sequence() == saved_sequence + 1,
            "T1 must finish and publish A through its original pointer before T2 can write");
    }
    else {
        copy.release();
        reader.done_reading();
        writer.write_commit(uint32_t{24680});
        cm::require(writer.get_leading_sequence() == saved_sequence + 1,
            "newly acquired ownership must be released so later writes can succeed");
    }
    cm::require(writer.c.reader_eviction_count == 0 && !reader.consume_eviction_notification(),
        "observation errors must invent no eviction or loss");
}

void unrelated_unknown_and_incarnation_mismatch()
{
    sintra::test::Temp_ring_dir directory("unrelated_unknown");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader reader(directory.str(), "raw", elements);
    Reader neighbor(directory.str(), "raw", elements);
    reader.start_reading();
    neighbor.start_reading();
    auto& control = writer.c;
    auto& slot = control.reading_sequences[reader.m_rs_index].data;
    auto& neighbor_slot = control.reading_sequences[neighbor.m_rs_index].data;
    const auto neighbor_state = neighbor_slot.word.load();
    const auto before = control.read_access.load();
    {
        Held_copy copy(reader);
        sintra::detail::process_identity_probe_hook = unknown_identity;
        sintra::test::checked_scavenge_orphans(control);
        // Force a scan for a different octile by installing an unpaired stale
        // count, an existing recoverable interrupted-guard state.
        const uint8_t free_octile = 1;
        control.read_access.fetch_add(sintra::octile_mask(free_octile));
        writer.advance_writer_octile_if_needed(elements / 8);
        sintra::detail::process_identity_probe_hook = nullptr;
        cm::require(slot.load_state().copying() && control.read_access == before,
            "unrelated UNKNOWN must permit safe reclamation/progress without clearing a marked guard");
    }
    slot.owner_start_stamp.fetch_add(1);
#if defined(__FreeBSD__)
    // A missed boot-time change can make the stamps of one live process
    // differ, so the mismatch is UNKNOWN and the slot stays with its owner.
    const auto reader_state = slot.word.load();
    const auto counts = control.read_access.load();
    cm::require(!sintra::test::checked_scavenge_orphans(control), "a live FreeBSD owner's stamp mismatch must reclaim nothing");
    cm::require(slot.word == reader_state && control.read_access == counts &&
        neighbor_slot.word == neighbor_state && !control.free_rs_stack.contains(reader.m_rs_index),
        "a live FreeBSD owner's stamp mismatch must preserve its slot, guard and count");
    slot.owner_start_stamp.fetch_sub(1);
    reader.done_reading();
#else
    cm::require(sintra::test::checked_scavenge_orphans(control), "a changed incarnation must reclaim the obsolete slot");
    cm::require(slot.status() == Writer::READER_STATE_INACTIVE &&
        neighbor_slot.word == neighbor_state &&
        cm::octile_count(control.read_access, neighbor_slot.load_state().guard_octile()) == 1,
        "incarnation mismatch must reclaim only the old owner and preserve its neighbor");
    reader.m_reading = false;
#endif
    neighbor.done_reading();
}

#if defined(__FreeBSD__)
// A wall-clock step moves the live copying reader's reported ki_start by the
// same amount as kern.boottime. Neither observation nor reclamation may take
// the reader for another incarnation.
void clock_step_preserves_copying_owner()
{
    sintra::test::Temp_ring_dir directory("clock_step_owner");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader reader(directory.str(), "raw", elements);
    reader.start_reading();
    auto& control = writer.c;
    auto& slot = control.reading_sequences[reader.m_rs_index].data;
    Held_copy copy(reader);
    const auto owner = slot.owner();
    const auto protected_word = slot.word.load();
    const auto protected_count = control.read_access.load();
    cm::require(State{protected_word}.copying() && State{protected_word}.guard_present(),
        "clock-step case must hold a real copying mark and its guard");

    sintra::process_identity_result_t observed{};
    {
        fakes::Scoped_fakes injected;
        fakes::s_clock_step = 3600;
        observed = sintra::probe_process_identity(owner);
        sintra::test::checked_scavenge_orphans(control);
    }
    cm::require(observed.status == sintra::Process_identity_status::LIVE && !observed.error,
        "a clock step must leave the live copying owner LIVE");
    cm::require(slot.word == protected_word && control.read_access == protected_count &&
        same_incarnation(slot.owner(), owner) && !control.free_rs_stack.contains(reader.m_rs_index),
        "a clock step must preserve COPYING, guard, count and slot ownership");
    copy.release();
    reader.done_reading();
}

// Lifecycle attachments compare the same stamps, and their live count decides
// whether the ring files are removed. A boot-time change reversed between the
// lookups must neither scavenge a live attachment nor drop it from that count.
void boot_time_change_keeps_attachments()
{
    sintra::test::Temp_ring_dir directory("boot_time_change_attachments");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader reader(directory.str(), "raw", elements);
    const auto attached = writer.count_live_attachments();
    size_t observed = 0;
    {
        fakes::Scoped_fakes injected;
        fakes::s_record_boot_time_shift = 3600;
        writer.scavenge_dead_attachments();
        observed = writer.count_live_attachments();
    }
    cm::require(attached >= 2 && observed == attached && writer.count_live_attachments() == attached,
        "a reversed boot-time change must keep every live attachment");
}
#endif

} // namespace

int main(int argc, char** argv)
{
    executable = std::filesystem::absolute(argv[0]).string();
    try {
        if (argc == 4 && std::strcmp(argv[1], "--death-child") == 0) {
            return cm::run_death_child(argv[2], std::atoi(argv[3]));
        }
        if (argc == 3 && std::strcmp(argv[1], "--child-acquire") == 0) {
            return run_acquisition_child(argv[2]);
        }
        if (argc == 2 && std::strcmp(argv[1], "--absence-only") == 0) {
            cm::marked_reader_death(executable, {.name = "marked_child_absence", .close_handles = true});
            std::puts("PASS marked_child_absence: every handle closed before first writer probe");
            return 0;
        }
        cm::marked_reader_death(executable, {.name = "marked_child_death", .timing = cm::Death_timing::WHILE_REQUESTED});
        std::puts("PASS marked_child_death");
        cm::run_dead_reader_cases(executable);
        acquisition_death_restores_capacity();
        std::puts("PASS acquisition_death_restores_capacity");
        unknown_reservation_rollback(false);
        std::puts("PASS first_reservation_unknown_rollback");
        unknown_reservation_rollback(true);
        std::puts("PASS inherited_reservation_unknown_rollback");
        unrelated_unknown_and_incarnation_mismatch();
        std::puts("PASS unrelated_unknown_and_incarnation_mismatch");
#if defined(__FreeBSD__)
        clock_step_preserves_copying_owner();
        std::puts("PASS clock_step_preserves_copying_owner");
        boot_time_change_keeps_attachments();
        std::puts("PASS boot_time_change_keeps_attachments");
#endif
        for (const auto& protection : k_protection_cases) {
            live_owner_protection(protection);
            std::printf("PASS live_owner_protection: %s\n", protection.name);
        }
        return 0;
    }
    catch (const std::exception& error) {
        sintra::detail::process_identity_probe_hook = nullptr;
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
        std::fprintf(stderr, "copying_mark_lifecycle_test: %s\n", error.what());
        return 1;
    }
}
