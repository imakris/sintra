// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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

namespace {

namespace cm = sintra::test::copying_mark;
using Writer = sintra::Ring_W<uint32_t>;
using Reader = sintra::Ring_R<uint32_t>;
using State = Writer::Reader_state_union;

std::string executable;
std::filesystem::path child_directory;
const char* child_pause_stage = nullptr;
std::atomic<bool> request_seen{false};
std::atomic<bool> writer_waiting{false};

void observe(const char* stage, const std::atomic<uint64_t>*, uint8_t octile)
{
    if (std::strcmp(stage, "request_published") == 0) {
        request_seen = true;
    }
    if (std::strcmp(stage, "writer_waiting") == 0) {
        writer_waiting = true;
    }
    if (child_pause_stage && std::strcmp(stage, child_pause_stage) == 0) {
        cm::signal_file(child_directory / "paused", std::to_string(octile));
        cm::wait_for_file(child_directory / "release");
    }
}

struct payload_t
{
    uint64_t sequence;
    sintra::message_string text;
};
using Frame = sintra::Message<payload_t, void, 0x72C90A1ull>;

void publish(sintra::Message_ring_W& writer, uint64_t sequence, const std::string& text)
{
    writer.write<Frame>(sintra::vb_size<Frame>(sequence, text), sequence, text);
    writer.done_writing();
}

unsigned count(uint64_t read_access, uint8_t octile)
{
    return static_cast<unsigned>((read_access >> (8 * octile)) & 0xffu);
}

template <typename Control>
int find_owner(Control& control, uint32_t pid)
{
    for (int index = 0; index < sintra::max_process_index; ++index) {
        if (control.reading_sequences[index].data.owner_pid == pid) {
            return index;
        }
    }
    throw std::runtime_error("child did not publish its reader-slot identity");
}

int run_child(const std::string& mode, const std::string& directory)
{
    child_directory = directory;
    sintra::detail::test_hooks::s_ring_guard_operation = observe;
    if (mode == "--child-copy") {
        sintra::Message_ring_R reader(directory, "req", 1);
        reader.start_reading();
        cm::signal_file(child_directory / "attached");
        child_pause_stage = "copy_validated";
        reader.fetch_message();
    }
    else {
        child_pause_stage = "slot_acquired";
        Reader reader(directory, "raw", sintra::test::pick_ring_elements<uint32_t>());
    }
    return 0;
}

void marked_child_death(bool require_absence)
{
    sintra::test::Temp_ring_dir directory("marked_child_death");
    sintra::Message_ring_W writer(directory.str(), "req", 1);
    sintra::Message_ring_R neighbor(directory.str(), "req", 1);
    neighbor.start_reading();
    cm::Test_child child(executable, {"--child-copy", directory.str()});
    cm::wait_for_file(directory.path / "attached");
    const std::string original = "neighbor owned frame survives exact reader reclamation";
    publish(writer, 1, original);
    const auto* held = static_cast<Frame*>(neighbor.fetch_message());
    cm::require(held && std::string(held->text) == original, "neighbor frame was not delivered");
    cm::wait_for_file(directory.path / "paused");
    auto& control = writer.c;
    const int child_index = find_owner(control, child.pid());
    auto& child_slot = control.reading_sequences[child_index].data;
    const auto marked = child_slot.load_state();
    const uint64_t child_stamp = child_slot.owner_start_stamp;
    cm::require(marked.copying() && marked.guard_present(), "child must die inside a marked real frame copy");
    cm::require(count(control.read_access, marked.guard_octile()) == 2,
        "child and neighbor must each own their initial count");

    neighbor.done_reading();
    const std::string filler(16384, 'p');
    while (writer.get_leading_sequence() < sintra::message_ring_size / 8) {
        publish(writer, 2, filler);
    }
    neighbor.start_reading();
    const int neighbor_index = neighbor.m_rs_index;
    const auto neighbor_state = control.reading_sequences[neighbor_index].data.load_state();
    cm::require(neighbor_state.guard_octile() != marked.guard_octile(),
        "neighbor must protect a distinct octile during dead-owner reclamation");

    bool expect_unknown_absence = false;
#if defined(__FreeBSD__)
    expect_unknown_absence = true;
#elif defined(__linux__)
    const int pidfd = sintra::detail::open_process_pidfd(::getpid());
    expect_unknown_absence = pidfd < 0;
    if (pidfd >= 0) {
        ::close(pidfd);
    }
#endif
#if defined(__FreeBSD__) || defined(__linux__)
    std::error_code absence_error;
#endif
    if (require_absence || expect_unknown_absence) {
        child.terminate(0);
        if (require_absence) {
            child.close();
        }
#ifdef _WIN32
        const bool absent = cm::wait_until([&]() {
            HANDLE remaining = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child.pid());
            const DWORD error = ::GetLastError();
            if (remaining) {
                ::CloseHandle(remaining);
            }
            return !remaining && error == ERROR_INVALID_PARAMETER;
        });
        cm::require(absent,
            "absence case must run with every parent, duplicate and harness handle closed");
#endif
#if defined(__FreeBSD__) || defined(__linux__)
        if (expect_unknown_absence) {
            const auto absence = sintra::probe_process_identity(child.pid(), child_stamp);
            cm::require(absence.status == sintra::Process_identity_status::UNKNOWN && absence.error,
                "reaped child must have an ambiguous native absence before writer probing");
            absence_error = absence.error;
        }
#endif
    }

    request_seen = false;
    sintra::detail::test_hooks::s_ring_guard_operation = observe;
    std::atomic<bool> writer_finished{false};
    std::exception_ptr writer_error;
    auto target = uint64_t(marked.guard_octile()) * (sintra::message_ring_size / 8);
    if (target <= writer.get_leading_sequence()) {
        target += sintra::message_ring_size;
    }
    std::thread writing([&]() {
        try {
            while (writer.get_leading_sequence() < target) {
                publish(writer, 3, filler);
            }
        }
        catch (...) {
            writer_error = std::current_exception();
        }
        writer_finished = true;
    });
    const bool observed_request = require_absence || expect_unknown_absence || cm::wait_until([&]() {
        return request_seen.load() || writer_finished.load();
    });
    if (!require_absence && !expect_unknown_absence) {
        child.terminate(259);
    }
    const bool progressed = cm::wait_until([&]() { return writer_finished.load(); });
    if (!progressed) {
        // A watchdog detects a failure; it never authorizes production reuse.
        neighbor.done_reading();
        control.scavenge_orphans();
    }
    writing.join();
    sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
#if defined(__FreeBSD__) || defined(__linux__)
    if (expect_unknown_absence) {
        bool unresolved_absence = false;
        if (writer_error) {
            try {
                std::rethrow_exception(writer_error);
            }
            catch (const std::system_error& error) {
                unresolved_absence = error.code() == absence_error;
            }
        }
        cm::require(observed_request && progressed && unresolved_absence,
            "ambiguous native absence must stop the blocked writer with its native error");
        cm::require(child_slot.word == marked.word &&
            child_slot.owner_pid == child.pid() && child_slot.owner_start_stamp == child_stamp &&
            count(control.read_access, marked.guard_octile()) == 1 &&
            count(control.read_access, neighbor_state.guard_octile()) == 1,
            "ambiguous native absence must retain the marked guard, count and owner tuple");
        cm::require(std::string(held->text) == original && held->sequence == 1 &&
            control.reader_eviction_count == 0,
            "ambiguous native absence must preserve the neighbor frame without invented loss");
        neighbor.done_reading();
        return;
    }
#endif
    cm::require(observed_request && progressed && !writer_error,
        "blocked writer must reclaim a killed marked child and finish");
    cm::require(require_absence || request_seen, "live marked child must force writer request arbitration");
    cm::require(sintra::probe_process_identity(child.pid(), child_stamp).status ==
        sintra::Process_identity_status::DEAD, "child incarnation must be proven DEAD");
    cm::require(child_slot.status() == Writer::READER_STATE_INACTIVE &&
        !child_slot.load_state().copying() && !child_slot.load_state().request_pending() &&
        child_slot.owner_pid == 0 && child_slot.owner_start_stamp == 0,
        "dead copying owner must have its flags and identity reclaimed");
    cm::require(count(control.read_access, marked.guard_octile()) == 0 &&
        count(control.read_access, neighbor_state.guard_octile()) == 1 &&
        control.reading_sequences[neighbor_index].data.load_state().word == neighbor_state.word,
        "death cleanup must release exactly the child's count and preserve its neighbor");
    cm::require(std::string(held->text) == original && held->sequence == 1,
        "neighbor's owned frame must remain intact through writer progress");
    sintra::Message_ring_R replacement(directory.str(), "req", 1);
    cm::require(replacement.m_rs_index == child_index,
        "reclaimed marked-child slot must be reusable");
    cm::require(control.guard_accounting_mismatch_count == 0, "count accounting must stay balanced");
    neighbor.done_reading();
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
    cm::require(control.scavenge_orphans(), "INACTIVE popped slot must report recovered capacity");
    cm::require(control.read_access == count_before, "inactive cleanup must preserve neighbor's owned count");
    cm::require(control.free_rs_stack.size() == sintra::max_process_index - 1,
        "death between pop and ACTIVE must restore the missing free entry");
    cm::require(!control.scavenge_orphans(), "repeated inactive cleanup must not duplicate free entries");
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

sintra::process_identity_result_t unknown_identity(uint32_t, uint64_t)
{
#ifdef _WIN32
    constexpr int error = ERROR_ACCESS_DENIED;
#else
    constexpr int error = EACCES;
#endif
    return {sintra::Process_identity_status::UNKNOWN, std::error_code(error, std::system_category())};
}

#ifdef _WIN32
sintra::process_identity_result_t denied_identity(uint32_t, uint64_t)
{
    return {sintra::Process_identity_status::UNKNOWN,
        std::error_code(ERROR_ACCESS_DENIED, std::system_category())};
}

void set_visibility_denial(bool)
{
    sintra::detail::process_identity_probe_hook = denied_identity;
}

void clear_visibility_denial()
{
    sintra::detail::process_identity_probe_hook = nullptr;
}
#elif defined(__linux__)
int hidden_stat_open(const char*, int, ...)
{
    errno = ENOENT;
    return -1;
}

int unavailable_pidfd(pid_t)
{
    errno = ENOSYS;
    return -1;
}

void set_visibility_denial(bool)
{
    sintra::detail::process_identity_open_stat = hidden_stat_open;
    sintra::detail::process_identity_pidfd_open = unavailable_pidfd;
}

void clear_visibility_denial()
{
    sintra::detail::process_identity_open_stat = ::open;
    sintra::detail::process_identity_pidfd_open = sintra::detail::open_process_pidfd;
}
#elif defined(__APPLE__)
int hidden_proc_pidinfo(int, int, uint64_t, void*, int)
{
    errno = ESRCH;
    return 0;
}

void set_visibility_denial(bool)
{
    sintra::detail::process_identity_proc_pidinfo = hidden_proc_pidinfo;
}

void clear_visibility_denial()
{
    sintra::detail::process_identity_proc_pidinfo = ::proc_pidinfo;
}
#elif defined(__FreeBSD__)
bool empty_process_record = false;

int hidden_process_sysctl(const int*, u_int, void*, size_t* size, const void*, size_t)
{
    if (empty_process_record) {
        *size = 0;
        return 0;
    }
    errno = ESRCH;
    return -1;
}

void set_visibility_denial(bool empty)
{
    empty_process_record = empty;
    sintra::detail::process_identity_sysctl = hidden_process_sysctl;
}

void clear_visibility_denial()
{
    sintra::detail::process_identity_sysctl = ::sysctl;
    empty_process_record = false;
}
#endif

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

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
void visibility_denial_preserves_copy(bool empty_record = false)
{
    sintra::test::Temp_ring_dir directory("visibility_denial");
    const size_t elements = sintra::test::pick_ring_elements<uint32_t>();
    Writer writer(directory.str(), "raw", elements);
    Reader reader(directory.str(), "raw", elements);
    reader.start_reading();
    auto& slot = writer.c.reading_sequences[reader.m_rs_index].data;
    const auto guarded_octile = slot.load_state().guard_octile();
    const auto target = guarded_octile == 0 ? elements : uint64_t(guarded_octile) * (elements / 8);
    fill_to(writer, target - 1);

    Held_copy copy(reader);
    const auto protected_word = slot.word.load();
    const auto protected_count = writer.c.read_access.load();
    const uint32_t owner_pid = slot.owner_pid.load();
    const uint64_t owner_stamp = slot.owner_start_stamp.load();
    cm::require(State{protected_word}.copying() && State{protected_word}.guard_present(),
        "visibility case must hold a real copying mark and its guard");

    set_visibility_denial(empty_record);
    const auto observed = sintra::probe_process_identity(owner_pid, owner_stamp);
    bool blocked = false;
    try {
        const uint32_t value = 45678;
        writer.write(&value, 1);
    }
    catch (const std::system_error& error) {
        blocked = error.code() == observed.error;
    }
    clear_visibility_denial();

    cm::require(observed.status == sintra::Process_identity_status::UNKNOWN && observed.error && blocked,
        "visibility denial must remain UNKNOWN and stop a blocked writer with its native error");
    cm::require(slot.word == protected_word && writer.c.read_access == protected_count &&
        slot.owner_pid == owner_pid && slot.owner_start_stamp == owner_stamp,
        "visibility denial must preserve COPYING, guard, count and exact owner identity");
    cm::require(writer.c.reader_eviction_count == 0 && !reader.consume_eviction_notification(),
        "visibility denial must not invent an eviction or lost range");
    copy.release();
    reader.done_reading();
}
#endif

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
        failed_natively = error.code() == unknown_identity(0, 0).error;
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
        control.scavenge_orphans();
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
    cm::require(control.scavenge_orphans(), "a changed incarnation must reclaim the obsolete slot");
    cm::require(slot.status() == Writer::READER_STATE_INACTIVE &&
        neighbor_slot.word == neighbor_state &&
        count(control.read_access, neighbor_slot.load_state().guard_octile()) == 1,
        "incarnation mismatch must reclaim only the old owner and preserve its neighbor");
    reader.m_reading = false;
    neighbor.done_reading();
}

} // namespace

int main(int argc, char** argv)
{
    executable = std::filesystem::absolute(argv[0]).string();
    try {
        if (argc == 3 && (std::strcmp(argv[1], "--child-copy") == 0 ||
            std::strcmp(argv[1], "--child-acquire") == 0))
        {
            return run_child(argv[1], argv[2]);
        }
        if (argc == 2 && std::strcmp(argv[1], "--absence-only") == 0) {
            marked_child_death(true);
            std::puts("PASS marked_child_absence: every handle closed before first writer probe");
            return 0;
        }
#if defined(__APPLE__)
        // A zombie can make proc_pidinfo fail while kill still sees the PID.
        // Reap first so this case exercises confirmed absence and reclamation.
        marked_child_death(true);
#else
        marked_child_death(false);
#endif
        std::puts("PASS marked_child_death");
        acquisition_death_restores_capacity();
        std::puts("PASS acquisition_death_restores_capacity");
        unknown_reservation_rollback(false);
        std::puts("PASS first_reservation_unknown_rollback");
        unknown_reservation_rollback(true);
        std::puts("PASS inherited_reservation_unknown_rollback");
        unrelated_unknown_and_incarnation_mismatch();
        std::puts("PASS unrelated_unknown_and_incarnation_mismatch");
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
        visibility_denial_preserves_copy();
        std::puts("PASS visibility_denial_preserves_copy");
#if defined(__FreeBSD__)
        visibility_denial_preserves_copy(true);
        std::puts("PASS empty_process_record_preserves_copy");
#endif
#endif
        return 0;
    }
    catch (const std::exception& error) {
        sintra::detail::process_identity_probe_hook = nullptr;
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
        std::fprintf(stderr, "copying_mark_lifecycle_test: %s\n", error.what());
        return 1;
    }
}
