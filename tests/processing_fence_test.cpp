#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>

namespace {

struct Work_message {};

struct Handler_done {};

constexpr auto k_handler_delay = std::chrono::milliseconds(300);
int controller_process()
{
    using namespace sintra;

    sintra::test::Shared_directory shared("SINTRA_PROCESSING_FENCE_DIR", "processing_fence");
    const auto shared_dir = shared.path();
    const auto result_path = shared_dir / "result.txt";

    const std::string group = "_sintra_external_processes";
    barrier("processing-fence-setup", group);

    auto handler_done_promise = std::make_shared<std::promise<void>>();
    auto handler_done_future = handler_done_promise->get_future();
    auto handler_done_recorded = std::make_shared<std::atomic<bool>>(false);

    auto handler_done_deactivator = activate_slot([
        handler_done_promise,
        handler_done_recorded
    ](const Handler_done&) {
        bool expected = false;
        if (handler_done_recorded->compare_exchange_strong(expected, true)) {
            handler_done_promise->set_value();
        }
    });

    world() << Work_message{};

    const auto start = std::chrono::steady_clock::now();
    const bool barrier_result = barrier<processing_fence_t>(
        "processing-fence", group);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // On Windows we occasionally observe a tiny delay between the barrier
    // returning and the handler completion notification becoming visible to
    // this thread. Allow a short grace period before declaring failure to make
    // the test robust against the observed scheduling jitter while still
    // validating that processing completed promptly.
    const auto handler_done_deadline =
        start + k_handler_delay + std::chrono::milliseconds(200);
    const bool handler_done = handler_done_future.wait_until(handler_done_deadline) ==
        std::future_status::ready;

    std::ofstream out(result_path, std::ios::binary | std::ios::trunc);
    out << (barrier_result && handler_done ? "ok" : "fail") << '\n';
    out << elapsed.count() << '\n';
    out << (handler_done ? "done" : "pending") << '\n';

    barrier("processing-fence-test-done", "_sintra_all_processes");

    handler_done_deactivator();

    return (barrier_result && handler_done) ? 0 : 1;
}

int worker_process()
{
    using namespace sintra;

    auto slot = [](const Work_message&) {
        std::this_thread::sleep_for(k_handler_delay);
        world() << Handler_done{};
    };
    activate_slot(slot);

    const std::string group = "_sintra_external_processes";
    barrier("processing-fence-setup", group);
    barrier<processing_fence_t>("processing-fence", group);
    barrier("processing-fence-test-done", "_sintra_all_processes");

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_PROCESSING_FENCE_DIR",
        "processing_fence",
        {controller_process, worker_process},
        [](const std::filesystem::path& shared_dir) {
            std::filesystem::remove(shared_dir / "result.txt");
        },
        [](const std::filesystem::path&) { return 0; },
        [](const std::filesystem::path& shared_dir) {
            const auto result_path = shared_dir / "result.txt";
            std::ifstream in(result_path, std::ios::binary);
            if (!in) {
                return 1;
            }

            std::string status;
            long long elapsed_ms = 0;
            std::string done_state;
            std::getline(in, status);
            in >> elapsed_ms;
            std::getline(in >> std::ws, done_state);

            const long long expected_ms = k_handler_delay.count();
            const bool elapsed_ok = elapsed_ms >= expected_ms;
            const bool done_ok = (done_state == "done");
            const bool success = (status == "ok") && elapsed_ok && done_ok;

            return success ? 0 : 1;
        },
        "processing-fence-test-done");
}
