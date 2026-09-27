#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "test_ring_utils.h"

namespace {

namespace fixture = sintra::test::copying_mark;
using Reader = sintra::Message_ring_R;
using Writer = sintra::Message_ring_W;

struct payload_t
{
    sintra::message_string text;
};

using Frame = sintra::Message<payload_t, void, 0x71A4E07ull>;
std::filesystem::path synchronization;
const std::atomic<uint64_t>* watched_access = nullptr;
std::atomic<bool> copy_paused{false};
std::atomic<bool> copy_release{false};
std::atomic<unsigned> releases{0};

void observe_writer(const char* stage, const std::atomic<uint64_t>*, uint8_t)
{
    if (std::string_view(stage) == "request_published") {
        fixture::signal_file(synchronization / "requested");
        fixture::wait_for_file(synchronization / "writer-release");
    }
}

void observe_reader(const char* stage, const std::atomic<uint64_t>* access, uint8_t)
{
    if (access != watched_access) {
        return;
    }
    if (std::string_view(stage) == "copy_validated") {
        copy_paused = true;
        copy_release.wait(false);
    }
    if (std::string_view(stage) == "release") {
        ++releases;
    }
}

int abandoned_writer(const char* directory, const char* sync, uint64_t process_id)
{
    synchronization = sync;
    Writer writer(directory, "req", process_id);
    fixture::signal_file(synchronization / "writer-ready");
    fixture::wait_for_file(synchronization / "start");
    sintra::detail::test_hooks::s_ring_guard_operation = &observe_writer;
    const std::string message(32768, 'f');
    for (unsigned i = 0; i < 256; ++i) {
        auto* frame = writer.write<Frame>(sintra::vb_size<Frame>(message), message);
        frame->sender_instance_id = process_id;
        writer.done_writing();
    }
    return 2;
}

void check_session(int argc, char* argv[])
{
    sintra::init(argc, argv);
    try {
        sintra::test::Temp_ring_dir sync("copy_teardown");
        const auto process_id = sintra::compose_instance(
            uint32_t(sintra::max_process_index - 1), 1ull);
        Writer replies(sintra::s_mproc->m_directory, "rep", process_id);
        fixture::Test_child writer(argv[0], {"--abandoned-writer", sintra::s_mproc->m_directory,
            sync.str(), std::to_string(process_id)});
        fixture::wait_for_file(sync.path / "writer-ready");
        auto progress = std::make_shared<sintra::Process_message_reader::Delivery_progress>();
        sintra::Process_message_reader reader(process_id, progress);
        fixture::require(reader.ready_for_test(), "production request/reply sessions must be ready");
        auto& ring = *reader.m_in_req_c;
        auto& slot = ring.c.reading_sequences[ring.m_rs_index].data;
        watched_access = &ring.c.read_access;
        copy_paused = false;
        copy_release = false;
        releases = 0;
        sintra::detail::test_hooks::s_ring_guard_operation = &observe_reader;
        fixture::signal_file(sync.path / "start");
        const bool paused = fixture::wait_until([]() { return copy_paused.load(); });
        fixture::wait_for_file(sync.path / "requested");
        const bool requested = true;
        writer.terminate();
        const auto before = slot.load_state();
        Reader neighbor(sintra::s_mproc->m_directory, "req", process_id);
        neighbor.start_reading();
        const auto neighbor_count = sintra::octile_mask(neighbor.m_trailing_octile);
        reader.stop_nowait();
        copy_release = true;
        copy_release.notify_all();
        const bool stopped = reader.stop_and_wait(5.0);
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
        watched_access = nullptr;
        const auto after = slot.load_state();
        fixture::require(paused && requested && before.copying() && before.request_pending(),
            "writer must be abandoned after REQUEST with an admitted production copy");
        fixture::require(stopped && progress->request_stopped && progress->reply_stopped,
            "production stop and session teardown must complete without a replacement writer");
        fixture::require(releases == 1 && !after.copying() && !after.guard_present() &&
            !after.guard_pending() && !after.request_pending() && ring.c.read_access == neighbor_count,
            "teardown must release exactly its own count and cancel pending REQUEST");
        fixture::require(slot.status() == Writer::READER_STATE_ACTIVE &&
            !ring.consume_eviction_notification() && ring.get_diagnostics().reader_eviction_count == 0,
            "normal stop must preserve neighboring protection without inventing eviction or loss");
        neighbor.done_reading();
    }
    catch (...) {
        copy_release = true;
        copy_release.notify_all();
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
        sintra::shutdown();
        throw;
    }
    sintra::shutdown();
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--abandoned-writer") {
            return abandoned_writer(argv[2], argv[3], std::stoull(argv[4]));
        }
        check_session(argc, argv);
        std::puts("PASS abandoned writer REQUEST: production stop releases one owned count");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "copying_mark_teardown_test: %s\n", error.what());
        return 1;
    }
}
