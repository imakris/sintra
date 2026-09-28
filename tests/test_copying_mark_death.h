// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// Death of a reader process inside a marked frame copy. Shared by the
// live-eviction and the disabled-eviction test executables: both must
// reclaim exactly the dead reader. Include after the Sintra messaging
// headers, with private members exposed.

#pragma once

#include "test_copying_mark_utils.h"
#include "test_process_identity_fakes.h"
#include "test_ring_utils.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sintra::test::copying_mark {

struct death_payload_t
{
    uint64_t sequence;
    sintra::message_string text;
};
using Death_frame = sintra::Message<death_payload_t, void, 0x72C90A1ull>;

inline void publish_death_frame(sintra::Message_ring_W& writer, uint64_t sequence, const std::string& text)
{
    writer.write<Death_frame>(sintra::vb_size<Death_frame>(sequence, text), sequence, text);
    writer.done_writing();
}

inline unsigned octile_count(uint64_t read_access, uint8_t octile)
{
    return static_cast<unsigned>((read_access >> (8 * octile)) & 0xffu);
}

template <typename Control>
int find_reader_slot(Control& control, uint32_t pid)
{
    for (int index = 0; index < sintra::max_process_index; ++index) {
        if (control.reading_sequences[index].data.owner_pid == pid) {
            return index;
        }
    }
    throw std::runtime_error("child did not publish its reader-slot identity");
}

// How a process's Linux namespaces appear to it through the injection seam,
// both when its reader publishes them and when it observes another reader.
// TIME_A and TIME_B keep the native PID namespace and simulate two time
// namespaces: joining one needs privileges that test hosts lack.
enum class Namespace_view
{
    NATIVE,
    ABSENT,
    UNKNOWN,
    TIME_A,
    TIME_B
};

enum class Death_timing
{
    WHILE_REQUESTED, // Killed after the live-eviction writer publishes REQUEST.
    BEFORE_WRITER    // Killed before the blocked writer's first probe.
};

struct death_case_t
{
    const char*    name;
    Death_timing   timing               = Death_timing::BEFORE_WRITER;
    bool           reap                 = true;  // POSIX: false leaves a zombie until reclamation is asserted.
    bool           close_handles        = false; // Windows: close every handle before the writer probes.
    bool           pidfd_unavailable    = false; // Linux: pidfd_open fails with ENOSYS.
    Namespace_view reader_view          = Namespace_view::NATIVE;
    Namespace_view observer_view        = Namespace_view::NATIVE;
    bool           observer_left_time_a = false; // Linux: the writer process observed in time namespace A first.
};

#if defined(__linux__)
inline constexpr uint64_t k_time_namespace_a = 0xa;
inline constexpr uint64_t k_time_namespace_b = 0xb;

inline std::optional<process_namespaces_t> namespaces_of_view(Namespace_view view)
{
    const auto unavailable = [](Process_metadata_state state) {
        return process_namespaces_t{{state, 0, 0}, {state, 0, 0}};
    };
    switch (view) {
        case Namespace_view::NATIVE:
            break;
        case Namespace_view::ABSENT:
            return unavailable(Process_metadata_state::ABSENT);
        case Namespace_view::UNKNOWN:
            return unavailable(Process_metadata_state::UNKNOWN);
        case Namespace_view::TIME_A:
        case Namespace_view::TIME_B: {
            auto namespaces = sintra::detail::current_linux_namespaces();
            namespaces.time = identity_fakes::valid_namespace(
                view == Namespace_view::TIME_A ? k_time_namespace_a : k_time_namespace_b);
            return namespaces;
        }
    }
    return std::nullopt;
}

// The observing process also sees its procfs view: absent metadata includes
// a kernel without NStgid, and unreadable metadata an unreadable status.
inline void apply_observer_view(Namespace_view view)
{
    namespace fakes = identity_fakes;
    fakes::s_namespaces = namespaces_of_view(view);
    fakes::s_status_error = view == Namespace_view::UNKNOWN ? EACCES : 0;
    if (view == Namespace_view::ABSENT) {
        fakes::use_status_nstgid("");
    }
}

inline void require_published_view(const process_namespaces_t& namespaces, Namespace_view view, const char* message)
{
    bool matches = true;
    switch (view) {
        case Namespace_view::NATIVE:
            break;
        case Namespace_view::ABSENT:
            matches = namespaces.pid.state == Process_metadata_state::ABSENT &&
                namespaces.time.state == Process_metadata_state::ABSENT;
            break;
        case Namespace_view::UNKNOWN:
            matches = namespaces.pid.state == Process_metadata_state::UNKNOWN &&
                namespaces.time.state == Process_metadata_state::UNKNOWN;
            break;
        case Namespace_view::TIME_A:
        case Namespace_view::TIME_B:
            matches = namespaces.time.state == Process_metadata_state::VALID &&
                namespaces.time.inode == (view == Namespace_view::TIME_A ? k_time_namespace_a : k_time_namespace_b);
            break;
    }
    require(matches, message);
}
#endif

inline std::filesystem::path s_death_directory;
inline std::atomic<bool> s_death_request_seen{false};

inline void pause_death_copy(const char* stage, const std::atomic<uint64_t>*, uint8_t octile)
{
    if (std::strcmp(stage, "copy_validated") == 0) {
        signal_file(s_death_directory / "paused", std::to_string(octile));
        wait_for_file(s_death_directory / "release");
    }
}

inline void observe_death_writer(const char* stage, const std::atomic<uint64_t>*, uint8_t)
{
    if (std::strcmp(stage, "request_published") == 0) {
        s_death_request_seen = true;
    }
}

// Child process: publish its identity in the given view, then stay inside a
// marked frame copy until it is killed.
inline int run_death_child(const std::string& directory, int view)
{
    s_death_directory = directory;
#if defined(__linux__)
    identity_fakes::Scoped_fakes fakes;
    identity_fakes::s_namespaces = namespaces_of_view(static_cast<Namespace_view>(view));
#else
    (void)view;
#endif
    sintra::Message_ring_R reader(directory, "req", 1);
    reader.start_reading();
    signal_file(s_death_directory / "attached");
    sintra::detail::test_hooks::s_ring_guard_operation = pause_death_copy;
    reader.fetch_message();
    return 0;
}

inline void marked_reader_death(const std::string& executable, const death_case_t& death)
{
    sintra::test::Temp_ring_dir directory("marked_reader_death");
    identity_fakes::Scoped_fakes fakes;
    sintra::Message_ring_W writer(directory.str(), "req", 1);
    auto& control = writer.c;
#if defined(__linux__)
    if (death.observer_left_time_a) {
        // Observe in time namespace A first: acquire and release a reader
        // slot, and classify a death from a process record.
        apply_observer_view(Namespace_view::TIME_A);
        {
            sintra::Message_ring_R earlier(directory.str(), "req", 1);
            auto earlier_owner = control.reading_sequences[earlier.m_rs_index].data.owner();
            require_published_view(earlier_owner.namespaces, Namespace_view::TIME_A,
                "a slot acquired in time namespace A must publish A");
            earlier_owner.start_stamp += 1;
            require(sintra::probe_process_identity(earlier_owner).status == Process_identity_status::DEAD,
                "another incarnation's record must be DEAD within one time namespace");
            control.scavenge_orphans();
        }
        require(control.free_rs_stack.size() == sintra::max_process_index,
            "every reader slot from time namespace A must be released");
    }
    apply_observer_view(death.observer_view);
    identity_fakes::s_pidfd_result = death.pidfd_unavailable ? ENOSYS : identity_fakes::k_native;
#endif

    sintra::Message_ring_R neighbor(directory.str(), "req", 1);
    neighbor.start_reading();
#if defined(__linux__)
    require_published_view(control.reading_sequences[neighbor.m_rs_index].data.owner().namespaces,
        death.observer_view, "each slot acquisition must publish its process's current namespaces");
#endif
    Test_child child(executable, {"--death-child", directory.str(), std::to_string(int(death.reader_view))});
    wait_for_file(directory.path / "attached");
    const std::string original = "neighbor owned frame survives exact reader reclamation";
    publish_death_frame(writer, 1, original);
    const auto* held = static_cast<Death_frame*>(neighbor.fetch_message());
    require(held && std::string(held->text) == original, "neighbor frame was not delivered");
    wait_for_file(directory.path / "paused");
    const int child_index = find_reader_slot(control, child.pid());
    auto& child_slot = control.reading_sequences[child_index].data;
    const auto marked = child_slot.load_state();
    const auto child_owner = child_slot.owner();
    require(marked.copying() && marked.guard_present(), "child must die inside a marked real frame copy");
    require(octile_count(control.read_access, marked.guard_octile()) == 2,
        "child and neighbor must each own their initial count");
#if defined(__linux__)
    require_published_view(child_owner.namespaces, death.reader_view,
        "child must publish the namespace metadata of its view");
#endif

    neighbor.done_reading();
    const std::string filler(16384, 'p');
    while (writer.get_leading_sequence() < sintra::message_ring_size / 8) {
        publish_death_frame(writer, 2, filler);
    }
    neighbor.start_reading();
    const int neighbor_index = neighbor.m_rs_index;
    const auto neighbor_state = control.reading_sequences[neighbor_index].data.load_state();
    require(neighbor_state.guard_octile() != marked.guard_octile(),
        "neighbor must protect a distinct octile during dead-owner reclamation");

    if (death.timing == Death_timing::BEFORE_WRITER) {
#ifdef _WIN32
        child.terminate(0);
        if (death.close_handles) {
            child.close();
            const bool absent = wait_until([&]() {
                HANDLE remaining = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child.pid());
                const DWORD error = ::GetLastError();
                if (remaining) {
                    ::CloseHandle(remaining);
                }
                return !remaining && error == ERROR_INVALID_PARAMETER;
            });
            require(absent, "absence case must run with every parent, duplicate and harness handle closed");
        }
#else
        if (death.reap) {
            child.terminate(0);
        }
        else {
            child.kill_unreaped();
        }
#endif
    }

    s_death_request_seen = false;
    sintra::detail::test_hooks::s_ring_guard_operation = observe_death_writer;
    std::atomic<bool> writer_finished{false};
    std::exception_ptr writer_error;
    auto target = uint64_t(marked.guard_octile()) * (sintra::message_ring_size / 8);
    if (target <= writer.get_leading_sequence()) {
        target += sintra::message_ring_size;
    }
    std::thread writing([&]() {
        try {
            while (writer.get_leading_sequence() < target) {
                publish_death_frame(writer, 3, filler);
            }
        }
        catch (...) {
            writer_error = std::current_exception();
        }
        writer_finished = true;
    });
    const bool requested = death.timing == Death_timing::WHILE_REQUESTED;
    const bool observed_request = !requested || wait_until([&]() {
        return s_death_request_seen.load() || writer_finished.load();
    });
    if (requested) {
        child.terminate(259);
    }
    const bool progressed = wait_until([&]() { return writer_finished.load(); });
    if (!progressed) {
        // A watchdog detects a failure; it never authorizes production reuse.
        neighbor.done_reading();
        control.scavenge_orphans();
    }
    writing.join();
    sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
    require(observed_request && progressed && !writer_error,
        "blocked writer must reclaim a killed marked child and finish");
    require(!requested || s_death_request_seen, "live marked child must force writer request arbitration");
    require(sintra::probe_process_identity(child_owner).status == Process_identity_status::DEAD,
        "child incarnation must be proven DEAD");
    const auto cleared = child_slot.owner();
    require(child_slot.status() == sintra::Message_ring_W::READER_STATE_INACTIVE &&
        !child_slot.load_state().copying() && !child_slot.load_state().request_pending() &&
        cleared.pid == 0 && cleared.start_stamp == 0 &&
        cleared.namespaces.pid.state == Process_metadata_state::UNKNOWN &&
        cleared.namespaces.pid.device == 0 && cleared.namespaces.pid.inode == 0 &&
        cleared.namespaces.time.state == Process_metadata_state::UNKNOWN &&
        cleared.namespaces.time.device == 0 && cleared.namespaces.time.inode == 0,
        "dead copying owner must have its flags and identity reclaimed");
    require(octile_count(control.read_access, marked.guard_octile()) == 0 &&
        octile_count(control.read_access, neighbor_state.guard_octile()) == 1 &&
        control.reading_sequences[neighbor_index].data.load_state().word == neighbor_state.word,
        "death cleanup must release exactly the child's count and preserve its neighbor");
    require(std::string(held->text) == original && held->sequence == 1,
        "neighbor's owned frame must remain intact through writer progress");
    sintra::Message_ring_R replacement(directory.str(), "req", 1);
    require(replacement.m_rs_index == child_index, "reclaimed marked-child slot must be reusable");
    require(control.guard_accounting_mismatch_count == 0, "count accounting must stay balanced");
#ifndef _WIN32
    if (!death.reap && death.timing == Death_timing::BEFORE_WRITER) {
        require(child.unreaped(), "child must stay unreaped until its reclamation has been asserted");
    }
#endif
    neighbor.done_reading();
}

// Readers killed before the blocked writer probes them. Each case must be
// reclaimed with live eviction enabled and disabled.
inline std::vector<death_case_t> dead_reader_cases()
{
    std::vector<death_case_t> cases;
#ifdef _WIN32
    cases.push_back({.name = "signaled_reader"});
#else
    cases.push_back({.name = "reaped_reader"});
    cases.push_back({.name = "unreaped_reader", .reap = false});
#if defined(__linux__)
    cases.push_back({.name = "reaped_reader_without_pidfd", .pidfd_unavailable = true});
    cases.push_back({
        .name              = "reaped_reader_without_pidfd_or_namespaces",
        .pidfd_unavailable = true,
        .reader_view       = Namespace_view::ABSENT,
        .observer_view     = Namespace_view::ABSENT});
    cases.push_back({
        .name              = "reaped_reader_without_pidfd_unreadable_namespaces",
        .pidfd_unavailable = true,
        .reader_view       = Namespace_view::UNKNOWN,
        .observer_view     = Namespace_view::UNKNOWN});
    cases.push_back({
        .name          = "unreaped_reader_without_namespaces",
        .reap          = false,
        .reader_view   = Namespace_view::ABSENT,
        .observer_view = Namespace_view::ABSENT});
    cases.push_back({
        .name          = "unreaped_reader_unreadable_namespaces",
        .reap          = false,
        .reader_view   = Namespace_view::UNKNOWN,
        .observer_view = Namespace_view::UNKNOWN});
    cases.push_back({
        .name                 = "unreaped_reader_after_observer_time_namespace_change",
        .reap                 = false,
        .reader_view          = Namespace_view::TIME_B,
        .observer_view        = Namespace_view::TIME_B,
        .observer_left_time_a = true});
#endif
#endif
    return cases;
}

inline void run_dead_reader_cases(const std::string& executable)
{
    for (const auto& death : dead_reader_cases()) {
        marked_reader_death(executable, death);
        std::printf("PASS %s\n", death.name);
    }
}

} // namespace sintra::test::copying_mark
