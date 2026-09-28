#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

#include "test_copying_mark_utils.h"
#include "test_copying_mark_death.h"
#include "test_ring_utils.h"

namespace {

namespace fixture = sintra::test::copying_mark;
using Reader = sintra::Message_ring_R;
using Writer = sintra::Message_ring_W;
using Frame = sintra::Message<sintra::message_string, void, 0x71A4E06ull>;

std::filesystem::path child_directory;
std::atomic<uint32_t> watched_pid{0};
std::atomic<unsigned> live_probes{0};
std::atomic<unsigned> requests{0};

void write_frame(Writer& writer, const std::string& text)
{
    writer.write<Frame>(sintra::vb_size<Frame>(text), text);
    writer.done_writing();
}

sintra::process_identity_result_t observe_identity(const sintra::process_incarnation_t& owner)
{
    const auto result = sintra::detail::probe_process_identity_native(owner);
    if (owner.pid == watched_pid && result.status == sintra::Process_identity_status::LIVE) {
        live_probes.fetch_add(1);
    }
    return result;
}

void observe_guard(const char* stage, const std::atomic<uint64_t>*, uint8_t)
{
    if (std::string_view(stage) == "request_published") {
        ++requests;
    }
    if (std::string_view(stage) == "copy_validated") {
        fixture::signal_file(child_directory / "paused");
        fixture::wait_for_file(child_directory / "release");
    }
}

int child(const std::string& directory, bool marked)
{
    child_directory = directory;
    Reader reader(directory, "req", 1);
    reader.start_reading();
    fixture::signal_file(child_directory / "ready");
    if (marked) {
        sintra::detail::test_hooks::s_ring_guard_operation = &observe_guard;
        reader.fetch_message();
    }
    else {
        fixture::signal_file(child_directory / "paused");
        fixture::wait_for_file(child_directory / "release");
    }
    return 0;
}

void run(const char* executable, bool marked)
{
    sintra::test::Temp_ring_dir directory("copy_no_eviction");
    Writer writer(directory.str(), "req", 1);
    write_frame(writer, "bootstrap");
    fixture::Test_child reader(executable, {"--child", directory.str(), marked ? "marked" : "guard"});
    fixture::wait_for_file(directory.path / "ready");
    write_frame(writer, "a complete frame");
    fixture::wait_for_file(directory.path / "paused");
    int index = -1;
    for (int i = 0; i < sintra::max_process_index; ++i) {
        if (writer.c.reading_sequences[i].data.owner_pid == reader.pid()) {
            index = i;
        }
    }
    fixture::require(index >= 0, "child must own a reader slot");
    auto& slot = writer.c.reading_sequences[index].data;
    watched_pid = reader.pid();
    live_probes = 0;
    requests = 0;
    sintra::detail::process_identity_probe_hook = &observe_identity;
    sintra::detail::test_hooks::s_ring_guard_operation = &observe_guard;
    std::atomic<bool> completed{false};
    std::exception_ptr error;
    std::thread producer([&]() {
        try {
            const auto start = writer.get_leading_sequence();
            const std::string filler(32768, 'f');
            while (writer.get_leading_sequence() - start < 2 * sintra::message_ring_size) {
                write_frame(writer, filler);
            }
        }
        catch (...) { error = std::current_exception(); }
        completed = true;
    });
    const bool scanned = fixture::wait_until([&]() { return live_probes >= 3 || completed; });
    const auto state = slot.load_state();
    const bool protected_owner = scanned && live_probes >= 3 && !completed &&
        state.guard_present() && state.copying() == marked && !state.request_pending() &&
        slot.status() == Writer::READER_STATE_ACTIVE && requests == 0 &&
        writer.get_diagnostics().reader_eviction_count == 0;
    reader.terminate(259);
    producer.join();
    sintra::detail::process_identity_probe_hook = nullptr;
    sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
    fixture::require(protected_owner, "numeric zero must protect live owners without publishing REQUEST");
    fixture::require(completed && !error && slot.status() == Writer::READER_STATE_INACTIVE &&
        writer.c.read_access == 0 && writer.get_diagnostics().reader_eviction_count == 0,
        "disabled eviction must periodically reclaim a dead published blocking guard");
    Reader reused(directory.str(), "req", 1);
    fixture::require(reused.m_rs_index == index, "dead reader slot must be reusable");
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--child") {
            return child(argv[2], std::string_view(argv[3]) == "marked");
        }
        if (argc == 4 && std::string_view(argv[1]) == "--death-child") {
            return fixture::run_death_child(argv[2], std::atoi(argv[3]));
        }
        run(argv[0], true);
        run(argv[0], false);
        std::puts("PASS numeric disabled eviction: live protection and dead blocking-guard reclamation");
        fixture::run_dead_reader_cases(std::filesystem::absolute(argv[0]).string());
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "copying_mark_no_eviction_test: %s\n", error.what());
        return 1;
    }
}
