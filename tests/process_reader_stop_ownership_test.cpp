#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>

namespace {

constexpr std::string_view k_prefix = "process_reader_stop_ownership_test: ";
std::thread::id s_stopping_thread;
std::atomic<const std::atomic<uint64_t>*> s_read_access[2]{};
std::atomic<unsigned> s_acquired_count{0};
std::atomic<unsigned> s_foreign_release_count{0};
std::atomic<unsigned> s_owner_release_count{0};

void observe_guard_ownership(
    const char* stage,
    const std::atomic<uint64_t>* read_access,
    uint8_t /*octile*/)
{
    const bool stopping_thread = std::this_thread::get_id() == s_stopping_thread;
    if (std::string_view(stage) == "acquired") {
        if (stopping_thread) {
            const unsigned index = s_acquired_count.fetch_add(1);
            if (index < 2) {
                s_read_access[index] = read_access;
            }
        }
        return;
    }
    if (read_access != s_read_access[0].load() && read_access != s_read_access[1].load()) {
        return;
    }
    if (stopping_thread) {
        s_foreign_release_count.fetch_add(1);
    }
    else {
        s_owner_release_count.fetch_add(1);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%.*sinit failed: %s\n", int(k_prefix.size()), k_prefix.data(), e.what());
        return 1;
    }

    bool ok = true;
    try {
        const auto process_id = sintra::compose_instance(
            uint32_t(sintra::max_process_index - 1), 1ull);
        sintra::Message_ring_W request_ring(sintra::s_mproc->m_directory, "req", process_id);
        sintra::Message_ring_W reply_ring(sintra::s_mproc->m_directory, "rep", process_id);
        auto progress = std::make_shared<sintra::Process_message_reader::Delivery_progress>();

        s_stopping_thread = std::this_thread::get_id();
        sintra::detail::test_hooks::s_ring_guard_operation = &observe_guard_ownership;
        sintra::Process_message_reader reader(process_id, progress);
        ok &= sintra::test::assert_true(reader.ready_for_test(), k_prefix,
            "construction must establish both reader sessions");

        reader.stop_nowait();
        const bool stopped = reader.stop_and_wait(2.0);
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;

        ok &= sintra::test::assert_true(stopped, k_prefix, "both reader threads must stop");
        ok &= sintra::test::assert_true(s_acquired_count.load() == 2, k_prefix,
            "both request and reply guards must be observed");
        ok &= sintra::test::assert_true(s_foreign_release_count.load() == 0, k_prefix,
            "the control thread must leave each guard to its reader thread");
        ok &= sintra::test::assert_true(s_owner_release_count.load() == 2, k_prefix,
            "each reader thread must release its own guard exactly once");
        ok &= sintra::test::assert_true(
            progress->request_stopped.load() && progress->reply_stopped.load(), k_prefix,
            "reader exit must publish final stopped delivery state");
    }
    catch (const std::exception& e) {
        sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
        std::fprintf(stderr, "%.*sfailed: %s\n", int(k_prefix.size()), k_prefix.data(), e.what());
        ok = false;
    }

    sintra::shutdown();
    return ok ? 0 : 1;
}
