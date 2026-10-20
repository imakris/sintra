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
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct ack_a_t {};
struct ack_b_t {};

struct data_message_t
{
    int    value;
    double score;
};

struct phase_two_message_t
{
    int    value;
    double score;
};

template <typename Value>
void wait_for_receive_slot()
{
    // Observe receive()'s real registration before releasing the senders,
    // using the same registry witness as receive_owned_test.
    using Message = sintra::Message<sintra::Enclosure<Value>>;
    const auto message_id = Message::id();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        {
            std::lock_guard<std::recursive_mutex> lock(sintra::s_mproc->m_handlers_mutex);
            const auto found = sintra::s_mproc->m_active_handlers.find(message_id);
            if (found != sintra::s_mproc->m_active_handlers.end()) {
                for (const auto& entry : found->second) {
                    if (!entry.second.empty()) {
                        return;
                    }
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "FAIL: receive slot was not installed\n");
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

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
    sintra::activate_slot([&](const ack_a_t&) {
        got_ack.store(true, std::memory_order_release);
    });

    sintra::barrier("sender-id-ready");
    sintra::barrier("start");

    const int    expected_value = 57;
    const double expected_score = 2.718;

    for (int value = expected_value; value < expected_value + 2; ++value) {
        sintra::world() << data_message_t{value, expected_score};
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !got_ack.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_ack.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "FAIL: timed out waiting for ack_a_t\n");
        std::abort();
    }

    sintra::barrier("phase-two");
    return 0;
}

int sender_b_process()
{
    std::atomic<bool> got_ack{false};

    sintra::activate_slot([&](const ack_b_t&) {
        got_ack.store(true, std::memory_order_release);
    });

    sintra::barrier("sender-id-ready");
    sintra::barrier("start");

    sintra::barrier("phase-two");

    const int    expected_value = 91;
    const double expected_score = 1.414;

    sintra::world() << phase_two_message_t{expected_value, expected_score};

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !got_ack.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_ack.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "FAIL: timed out waiting for ack_b_t\n");
        std::abort();
    }

    return 0;
}

int receiver_process()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "receive_test");
    const auto shared_dir = shared.path();

    sintra::barrier("sender-id-ready");
    const auto sender_a_id = read_sender_id(shared_dir / "sender_a_id.txt");

    data_message_t msg{};
    std::thread receiver([&] {
        msg = sintra::receive<data_message_t>(
            sintra::Typed_instance_id<sintra::Managed_process>(sender_a_id));
    });
    wait_for_receive_slot<data_message_t>();
    sintra::barrier("start");
    receiver.join();
    sintra::test::write_lines(
        shared_dir / "received_value.txt",
        {std::to_string(msg.value)});
    if (std::fabs(msg.score - 2.718) > 0.02) {
        std::fprintf(stderr, "FAIL: expected filtered score near 2.718, got %f\n", msg.score);
        std::abort();
    }
    sintra::world() << ack_a_t{};

    phase_two_message_t other_msg{};
    std::thread unfiltered_receiver([&] {
        other_msg = sintra::receive<phase_two_message_t>();
    });
    wait_for_receive_slot<phase_two_message_t>();
    sintra::barrier("phase-two");
    unfiltered_receiver.join();

    if (other_msg.value != 91) {
        std::fprintf(stderr, "FAIL: expected second value 91, got %d\n", other_msg.value);
        std::abort();
    }
    if (std::fabs(other_msg.score - 1.414) > 0.02) {
        std::fprintf(stderr, "FAIL: expected second score near 1.414, got %f\n", other_msg.score);
        std::abort();
    }
    sintra::world() << ack_b_t{};

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

    sintra::shutdown();

    if (is_coordinator) {
        const auto received_values =
            sintra::test::read_lines(shared_dir_raii.path() / "received_value.txt");
        const std::string failure_message = received_values.empty()
            ? "receiver did not record a matching burst message"
            : "receive() returned later burst value " + received_values.front();
        const bool ok = sintra::test::assert_true(
            received_values.size() == 1 && received_values.front() == "57",
            "receive_test: ",
            failure_message);
        if (ok) {
            std::fprintf(stderr, "receive test PASSED\n");
        }
        return ok ? 0 : 1;
    }

    return 0;
}
