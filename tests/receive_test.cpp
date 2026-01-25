//
// Sintra receive<MESSAGE_T>() Test
//
// This test validates the receive<MESSAGE_T>() synchronous message receiving function.
//

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct Stop {};
struct Ack {};
struct StopAck {};

struct DataMessage {
    int value;
    double score;
};

inline int current_pid()
{
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

inline uint64_t thread_id_numeric()
{
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

inline uint64_t elapsed_ms()
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

inline std::mutex& log_mutex()
{
    static std::mutex mtx;
    return mtx;
}

inline void log_event(const char* role, const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[receive_test +%llums pid=%d branch=%d tid=%llu] %s: ",
                 static_cast<unsigned long long>(elapsed_ms()),
                 current_pid(),
                 sintra::detail::process_index(),
                 static_cast<unsigned long long>(thread_id_numeric()),
                 role);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

int sender_process()
{
    std::atomic<bool> got_ack{false};
    std::atomic<bool> got_stop_ack{false};
    std::uint64_t data_sent = 0;
    std::uint64_t stop_sent = 0;

    log_event("sender", "starting sender process");

    sintra::activate_slot([&](const Ack&) {
        got_ack.store(true, std::memory_order_release);
        log_event("sender", "received Ack");
    });

    sintra::activate_slot([&](const StopAck&) {
        got_stop_ack.store(true, std::memory_order_release);
        log_event("sender", "received StopAck");
    });

    log_event("sender", "waiting at start barrier");
    sintra::barrier("start");
    log_event("sender", "passed start barrier");

    const int expected_value = 57;
    const double expected_score = 2.718;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto last_log = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        if (got_ack.load(std::memory_order_acquire)) {
            break;
        }
        sintra::world() << DataMessage{expected_value, expected_score};
        ++data_sent;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::milliseconds(250)) {
            last_log = now;
            log_event("sender", "awaiting Ack (data_sent=%llu, got_ack=%d)",
                      static_cast<unsigned long long>(data_sent),
                      got_ack.load(std::memory_order_acquire) ? 1 : 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_ack.load(std::memory_order_acquire)) {
        log_event("sender", "FAIL: timed out waiting for Ack (data_sent=%llu)",
                  static_cast<unsigned long long>(data_sent));
        std::abort();
    }

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    last_log = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        if (got_stop_ack.load(std::memory_order_acquire)) {
            break;
        }
        sintra::world() << Stop{};
        ++stop_sent;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::milliseconds(250)) {
            last_log = now;
            log_event("sender", "awaiting StopAck (stop_sent=%llu, got_stop_ack=%d)",
                      static_cast<unsigned long long>(stop_sent),
                      got_stop_ack.load(std::memory_order_acquire) ? 1 : 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_stop_ack.load(std::memory_order_acquire)) {
        log_event("sender", "FAIL: timed out waiting for StopAck (data_sent=%llu stop_sent=%llu)",
                  static_cast<unsigned long long>(data_sent),
                  static_cast<unsigned long long>(stop_sent));
        std::abort();
    }

    log_event("sender", "waiting at done barrier");
    sintra::barrier("done", "_sintra_all_processes");
    log_event("sender", "passed done barrier");
    return 0;
}

int receiver_process()
{
    log_event("receiver", "starting receiver process");
    log_event("receiver", "waiting at start barrier");
    sintra::barrier("start");
    log_event("receiver", "passed start barrier");

    log_event("receiver", "waiting for DataMessage");
    auto msg = sintra::receive<DataMessage>();
    log_event("receiver", "received DataMessage value=%d score=%f", msg.value, msg.score);
    if (msg.value != 57) {
        log_event("receiver", "FAIL: expected 57, got %d", msg.value);
        std::abort();
    }
    if (std::fabs(msg.score - 2.718) > 0.02) {
        log_event("receiver", "FAIL: expected score near 2.718, got %f", msg.score);
        std::abort();
    }
    log_event("receiver", "sending Ack");
    sintra::world() << Ack{};

    log_event("receiver", "waiting for Stop");
    sintra::receive<Stop>();
    log_event("receiver", "received Stop, sending StopAck");
    sintra::world() << StopAck{};

    log_event("receiver", "waiting at done barrier");
    sintra::barrier("done", "_sintra_all_processes");
    log_event("receiver", "passed done barrier");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // Check if this is a spawned child process
    const bool is_coordinator = !std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    log_event(is_coordinator ? "coord" : "child", "init: argc=%d", argc);
    sintra::init(argc, const_cast<const char* const*>(argv),
                 sender_process, receiver_process);

    if (is_coordinator) {
        log_event("coord", "waiting at done barrier");
        sintra::barrier("done", "_sintra_all_processes");
        log_event("coord", "passed done barrier");
    }

    sintra::finalize();

    if (is_coordinator) {
        log_event("coord", "receive test PASSED");
    }

    return 0;
}
