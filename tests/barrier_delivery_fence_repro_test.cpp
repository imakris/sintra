//
// Sintra Delivery Fence Regression Reproducer
//
// Exercises the delivery-fence barrier semantics under heavy backlog. Workers
// emit bursts of markers and immediately enter a delivery-fence barrier while
// the coordinator handles the markers very slowly. The coordinator trusts the
// barrier to flush all pre-barrier messages and checks after the barrier returns
// whether every reader observed the complete burst. Any gap indicates that the
// delivery fence released too early.
//

#include <sintra/sintra.h>

#include "test_environment.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kWorkerCount  = 2;
constexpr std::size_t kIterations   = 64;
constexpr std::size_t kBurstCount   = 8;
constexpr auto kHandlerDelay        = std::chrono::milliseconds(12);

struct Iteration_marker
{
    std::uint32_t worker;
    std::uint32_t iteration;
    std::uint32_t sequence;
};

constexpr std::string_view kEnvSharedDir = "SINTRA_DELIVERY_FENCE_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_DELIVERY_FENCE_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("barrier_delivery_fence");

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "delivery_fence_repro_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

struct Coordinator_state
{
    std::mutex mutex;
    std::condition_variable cv;
    std::array<std::uint32_t, kWorkerCount> next_expected_sequence{};
    std::array<std::uint32_t, kWorkerCount> next_expected_iteration{};
    std::array<std::uint32_t, kWorkerCount> messages_seen_in_iteration{};
    std::size_t messages_in_iteration = 0;
    std::size_t total_messages        = 0;
    bool first_message_arrived        = false;
    bool iteration_failed             = false;
    std::string failure_detail;
};

void write_result(const std::filesystem::path& dir,
                  bool success,
                  std::size_t iterations_completed,
                  std::size_t total_messages,
                  const std::string& failure_reason)
{
    std::ofstream out(dir / "delivery_fence_repro_result.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open delivery_fence_repro_result.txt for writing");
    }

    out << (success ? "ok" : "fail") << '\n';
    out << iterations_completed << '\n';
    out << total_messages << '\n';
    if (!success) {
        out << failure_reason << '\n';
    }
}

int coordinator_process()
{
    using namespace sintra;

    Coordinator_state state;
    bool success = true;
    std::string failure_reason;
    std::size_t iterations_completed = 0;
    bool aborted = false;

    activate_slot([&](const Iteration_marker& marker) {
        std::unique_lock<std::mutex> lock(state.mutex);

        auto mark_failure = [&](const std::string& reason) {
            if (!state.iteration_failed) {
                state.iteration_failed = true;
                state.failure_detail = reason;
            }
        };

        if (marker.worker >= kWorkerCount) {
            std::ostringstream oss;
            oss << "Invalid worker index " << marker.worker << " (expected < " << kWorkerCount << ')';
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        auto& expected_iteration = state.next_expected_iteration[marker.worker];
        auto& expected_sequence = state.next_expected_sequence[marker.worker];
        auto& messages_seen = state.messages_seen_in_iteration[marker.worker];

        if (marker.iteration != expected_iteration) {
            std::ostringstream oss;
            oss << "Worker " << marker.worker << " expected iteration " << expected_iteration
                << " but sent " << marker.iteration;
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        if (marker.sequence != expected_sequence) {
            std::ostringstream oss;
            oss << "Worker " << marker.worker << " expected sequence " << expected_sequence
                << " but sent " << marker.sequence;
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        const bool first_message = (state.messages_in_iteration == 0);

        lock.unlock();
        std::this_thread::sleep_for(kHandlerDelay);
        lock.lock();

        ++expected_sequence;
        ++messages_seen;
        ++state.messages_in_iteration;
        ++state.total_messages;

        if (messages_seen == kBurstCount) {
            messages_seen = 0;
            ++expected_iteration;
        }

        if (first_message) {
            state.first_message_arrived = true;
        }

        state.cv.notify_all();
    });

    barrier("delivery-fence-repro-ready");

    constexpr auto first_marker_timeout = std::chrono::seconds(5);
    constexpr auto drain_timeout        = std::chrono::seconds(10);

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        bool barrier_called = false;

        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.messages_in_iteration = 0;
            state.first_message_arrived = false;
            state.iteration_failed = false;
            state.failure_detail.clear();
        }

        if (!aborted) {
            std::unique_lock<std::mutex> lock(state.mutex);
            const bool first_arrived = state.cv.wait_for(lock, first_marker_timeout, [&] {
                return state.first_message_arrived || state.iteration_failed;
            });

            if (!first_arrived && !state.iteration_failed) {
                if (success) {
                    std::ostringstream oss;
                    oss << "Coordinator did not observe first marker for iteration " << iteration;
                    failure_reason = oss.str();
                }
                success = false;
                aborted = true;
            }
            else if (state.iteration_failed) {
                if (success) {
                    failure_reason = state.failure_detail.empty()
                        ? "Coordinator observed invalid marker sequence"
                        : state.failure_detail;
                }
                success = false;
                aborted = true;
            }
            lock.unlock();

            if (!aborted) {
                const auto barrier_result = sintra::barrier("delivery-fence-repro-iteration");
                barrier_called = true;
                if (!barrier_result.succeeded()) {
                    if (success) {
                        failure_reason = "Coordinator barrier rendezvous failed";
                    }
                    success = false;
                    aborted = true;
                }
            }

            lock.lock();
            if (!aborted && !state.iteration_failed) {
                std::vector<std::size_t> missing_workers;
                for (std::size_t worker = 0; worker < kWorkerCount; ++worker) {
                    if (state.next_expected_iteration[worker] <= iteration) {
                        missing_workers.push_back(worker);
                    }
                }

                if (!missing_workers.empty()) {
                    if (success) {
                        std::ostringstream oss;
                        oss << "Barrier released before delivery completed for iteration " << iteration
                            << ". Missing workers:";
                        for (std::size_t idx = 0; idx < missing_workers.size(); ++idx) {
                            oss << (idx == 0 ? " " : " ") << missing_workers[idx];
                        }
                        failure_reason = oss.str();
                    }
                    success = false;
                    aborted = true;
                }
            }

            if (!aborted && !state.iteration_failed) {
                const bool drained = state.cv.wait_for(lock, drain_timeout, [&] {
                    for (std::size_t worker = 0; worker < kWorkerCount; ++worker) {
                        if (state.next_expected_iteration[worker] <= iteration) {
                            return false;
                        }
                    }
                    return true;
                });

                if (!drained) {
                    if (success) {
                        std::ostringstream oss;
                        oss << "Iteration " << iteration << " did not drain after barrier";
                        failure_reason = oss.str();
                    }
                    success = false;
                    aborted = true;
                }
            }

            lock.unlock();

            if (!aborted) {
                ++iterations_completed;
            }
        }

        if (!barrier_called) {
            sintra::barrier("delivery-fence-repro-iteration");
        }
    }

    barrier("delivery-fence-repro-done", "_sintra_all_processes");
    deactivate_all_slots();

    const auto shared_dir = get_shared_directory();
    write_result(shared_dir, success, iterations_completed, state.total_messages, failure_reason);
    return success ? 0 : 1;
}

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    barrier("delivery-fence-repro-ready");

    std::uint32_t sequence = 0;

    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {

        for (std::uint32_t burst = 0; burst < kBurstCount; ++burst) {
            world() << Iteration_marker{worker_index, iteration, sequence};
            ++sequence;

            if ((burst + worker_index + iteration) % 3 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }

        barrier("delivery-fence-repro-iteration");
    }

    barrier("delivery-fence-repro-done", "_sintra_all_processes");
    return 0;
}

int worker0_process()
{
    return worker_process(0);
}

int worker1_process()
{
    return worker_process(1);
}

void custom_terminate_handler()
{
    std::fprintf(stderr, "std::terminate called!\n");
    std::fprintf(stderr, "Uncaught exceptions: %d\n", std::uncaught_exceptions());

    try {
        auto eptr = std::current_exception();
        if (eptr) {
            std::rethrow_exception(eptr);
        } else {
            std::fprintf(stderr, "terminate called without an active exception\n");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Uncaught exception: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Uncaught exception of unknown type\n");
    }

    std::abort();
}

void cleanup_directory_with_retries(const std::filesystem::path& dir)
{
    bool cleanup_succeeded = false;
    for (int retry = 0; retry < 3 && !cleanup_succeeded; ++retry) {
        try {
            std::filesystem::remove_all(dir);
            cleanup_succeeded = true;
        } catch (const std::exception& e) {
            if (retry < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                std::fprintf(stderr,
                             "Warning: failed to remove temp directory %s after 3 attempts: %s\n",
                             dir.string().c_str(), e.what());
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(custom_terminate_handler);

    const bool is_spawned = [] (int argc, char* argv[]) {
        for (int i = 0; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--branch_index") {
                return true;
            }
        }
        return false;
    }(argc, argv);

    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(worker0_process);
    processes.emplace_back(worker1_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("delivery-fence-repro-done", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto result_path = shared_dir / "delivery_fence_repro_result.txt";
        if (!std::filesystem::exists(result_path)) {
            std::fprintf(stderr, "Error: result file not found at %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
            return 1;
        }

        std::ifstream in(result_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "Error: failed to open result file %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
            return 1;
        }

        std::string status;
        std::size_t iterations_completed = 0;
        std::size_t total_messages = 0;
        std::string reason;

        std::getline(in, status);
        in >> iterations_completed;
        in >> total_messages;
        std::getline(in >> std::ws, reason);
        in.close();

        cleanup_directory_with_retries(shared_dir);

        if (status != "ok") {
            std::fprintf(stderr, "Delivery fence regression repro reported failure: %s\n", reason.c_str());
            return 1;
        }
        if (iterations_completed != kIterations) {
            std::fprintf(stderr, "Expected %zu iterations, got %zu\n",
                         kIterations, iterations_completed);
            return 1;
        }
        const std::size_t expected_messages = kWorkerCount * kIterations * kBurstCount;
        if (total_messages != expected_messages) {
            std::fprintf(stderr, "Expected %zu total messages, got %zu\n",
                         expected_messages, total_messages);
            return 1;
        }
    }

    return 0;
}
