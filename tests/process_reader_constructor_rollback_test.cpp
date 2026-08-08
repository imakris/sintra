#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_prefix =
    "process_reader_constructor_rollback_test: ";

std::atomic<int>         s_baseline_active_readers{0};
std::atomic<bool>        s_request_reader_started{false};
std::atomic<const char*> s_failure_stage{nullptr};

int active_reader_count()
{
    std::lock_guard<std::mutex> lock(
        sintra::s_mproc->m_num_active_readers_mutex);
    return sintra::s_mproc->m_num_active_readers;
}

bool inject_reader_failure(
    const char*              stage,
    sintra::instance_id_type /*process_instance_id*/,
    uint32_t                 /*occurrence*/) noexcept
{
    const char* selected_stage =
        s_failure_stage.load(std::memory_order_acquire);
    if (!selected_stage || std::string_view(stage) != selected_stage) {
        return false;
    }

    if (std::string_view(stage) == sintra::detail::test_hooks::
            k_process_reader_reply_thread_creation)
    {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (active_reader_count() >
                s_baseline_active_readers.load(std::memory_order_acquire))
            {
                s_request_reader_started.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
    }
    return true;
}

bool verify_failed_construction(
    sintra::instance_id_type process_instance_id,
    const char*              failure_stage)
{
    s_failure_stage.store(failure_stage, std::memory_order_release);
    sintra::detail::test_hooks::s_process_reader_failure.store(
        &inject_reader_failure, std::memory_order_release);

    bool caught_expected_failure = false;
    try {
        auto progress =
            std::make_shared<sintra::Process_message_reader::Delivery_progress>();
        sintra::Process_message_reader reader(process_instance_id, progress);
    }
    catch (const std::system_error& e) {
        caught_expected_failure =
            e.code() == std::make_error_code(
                std::errc::resource_unavailable_try_again);
    }
    catch (...) {
    }

    sintra::detail::test_hooks::s_process_reader_failure.store(
        nullptr, std::memory_order_release);
    s_failure_stage.store(nullptr, std::memory_order_release);

    const int active_after_failure = active_reader_count();
    const int baseline =
        s_baseline_active_readers.load(std::memory_order_acquire);
    if (active_after_failure != baseline) {
        std::fprintf(stderr,
            "%.*sfailed construction leaked an active reader "
            "at %s (baseline=%d, current=%d, request_started=%d)\n",
            static_cast<int>(k_prefix.size()),
            k_prefix.data(),
            failure_stage,
            baseline,
            active_after_failure,
            s_request_reader_started.load(std::memory_order_acquire) ? 1 : 0);
        std::fflush(stderr);
        std::_Exit(1);
    }

    return caught_expected_failure;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%.*sinit failed: %s\n",
            static_cast<int>(k_prefix.size()), k_prefix.data(), e.what());
        return 1;
    }

    bool ok = true;
    s_baseline_active_readers.store(
        active_reader_count(), std::memory_order_release);

    const auto synthetic_process_id = sintra::compose_instance(
        static_cast<uint32_t>(sintra::max_process_index - 1), 1ull);
    sintra::Message_ring_W request_ring(
        sintra::s_mproc->m_directory, "req", synthetic_process_id);
    sintra::Message_ring_W reply_ring(
        sintra::s_mproc->m_directory, "rep", synthetic_process_id);

    ok &= sintra::test::assert_true(
        verify_failed_construction(
            synthetic_process_id,
            sintra::detail::test_hooks::
                k_process_reader_reply_thread_creation),
        k_prefix,
        "reply-thread creation should fail with resource_unavailable_try_again");
    ok &= sintra::test::assert_true(
        verify_failed_construction(
            synthetic_process_id,
            sintra::detail::test_hooks::k_process_reader_reply_session_start),
        k_prefix,
        "reply-session startup should roll back the request session");

    try {
        auto progress =
            std::make_shared<sintra::Process_message_reader::Delivery_progress>();
        {
            sintra::Process_message_reader reader(synthetic_process_id, progress);
            ok &= sintra::test::assert_true(
                reader.ready_for_test(),
                k_prefix,
                "successful construction should publish both lifetime guards");
        }
        ok &= sintra::test::assert_true(
            active_reader_count() ==
                s_baseline_active_readers.load(std::memory_order_acquire),
            k_prefix,
            "immediate post-construction destruction should stop both readers");
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%.*spost-rollback construction failed: %s\n",
            static_cast<int>(k_prefix.size()), k_prefix.data(), e.what());
        ok = false;
    }

    sintra::shutdown();
    return ok ? 0 : 1;
}
