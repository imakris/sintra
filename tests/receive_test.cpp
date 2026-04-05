//
// Sintra receive<MESSAGE_T>() Test
//
// This test validates the receive<MESSAGE_T>() synchronous message receiving function.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <thread>

namespace {

struct Ack_a {};
struct Ack_b {};

struct DataMessage {
    int value;
    double score;
};

void write_sender_id(const std::filesystem::path& path, sintra::instance_id_type instance_id)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << static_cast<unsigned long long>(instance_id) << '\n';
}

sintra::instance_id_type read_sender_id(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    unsigned long long raw = 0;
    in >> raw;
    if (!in) {
        std::fprintf(stderr, "FAIL: could not read sender id from %s\n", path.string().c_str());
        std::abort();
    }
    return static_cast<sintra::instance_id_type>(raw);
}

int sender_a_process()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "receive_test");
    const auto shared_dir = shared.path();
    write_sender_id(shared_dir / "sender_a_id.txt", sintra::process_of(sintra::s_mproc_id));

    std::atomic<bool> got_ack{false};
    sintra::activate_slot([&](const Ack_a&) {
        got_ack.store(true, std::memory_order_release);
    });

    sintra::barrier("sender-id-ready");
    sintra::barrier("start");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int expected_value = 57;
    const double expected_score = 2.718;

    sintra::world() << DataMessage{expected_value, expected_score};

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !got_ack.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_ack.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "FAIL: timed out waiting for Ack_a\n");
        std::abort();
    }

    sintra::barrier("phase-two");
    sintra::barrier("done", "_sintra_all_processes");
    return 0;
}

int sender_b_process()
{
    std::atomic<bool> got_ack{false};

    sintra::activate_slot([&](const Ack_b&) {
        got_ack.store(true, std::memory_order_release);
    });

    sintra::barrier("sender-id-ready");
    sintra::barrier("start");

    sintra::barrier("phase-two");

    const int expected_value = 91;
    const double expected_score = 1.414;

    sintra::world() << DataMessage{expected_value, expected_score};

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !got_ack.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_ack.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "FAIL: timed out waiting for Ack_b\n");
        std::abort();
    }

    sintra::barrier("done", "_sintra_all_processes");
    return 0;
}

int receiver_process()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "receive_test");
    const auto shared_dir = shared.path();

    sintra::barrier("sender-id-ready");
    sintra::barrier("start");

    const auto sender_a_id = read_sender_id(shared_dir / "sender_a_id.txt");

    auto msg = sintra::receive<DataMessage>(
        sintra::Typed_instance_id<sintra::Managed_process>(sender_a_id));
    if (msg.value != 57) {
        std::fprintf(stderr, "FAIL: expected filtered value 57, got %d\n", msg.value);
        std::abort();
    }
    if (std::fabs(msg.score - 2.718) > 0.02) {
        std::fprintf(stderr, "FAIL: expected filtered score near 2.718, got %f\n", msg.score);
        std::abort();
    }
    sintra::world() << Ack_a{};

    std::condition_variable second_cv;
    std::mutex second_mtx;
    std::optional<DataMessage> second_message;
    auto second_message_slot = sintra::activate_slot([&](DataMessage msg) {
        std::lock_guard<std::mutex> lock(second_mtx);
        if (!second_message.has_value()) {
            second_message.emplace(std::move(msg));
            second_cv.notify_one();
        }
    });

    sintra::barrier("phase-two");

    DataMessage other_msg{};
    {
        std::unique_lock<std::mutex> lock(second_mtx);
        second_cv.wait(lock, [&] { return second_message.has_value(); });
        other_msg = std::move(*second_message);
    }
    second_message_slot();

    if (other_msg.value != 91) {
        std::fprintf(stderr, "FAIL: expected second value 91, got %d\n", other_msg.value);
        std::abort();
    }
    if (std::fabs(other_msg.score - 1.414) > 0.02) {
        std::fprintf(stderr, "FAIL: expected second score near 1.414, got %f\n", other_msg.score);
        std::abort();
    }
    sintra::world() << Ack_b{};

    sintra::barrier("done", "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // Check if this is a spawned child process
    const bool is_coordinator = !std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });
    sintra::test::Shared_directory shared_dir_raii("SINTRA_TEST_SHARED_DIR", "receive_test");

    sintra::init(argc, const_cast<const char* const*>(argv),
                 sender_a_process, sender_b_process, receiver_process);

    if (is_coordinator) {
        sintra::barrier("done", "_sintra_all_processes");
    }

    sintra::finalize();

    if (is_coordinator) {
        std::fprintf(stderr, "receive test PASSED\n");
    }

    return 0;
}
