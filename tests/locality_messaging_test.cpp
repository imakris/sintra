//
// Sintra Locality-Based Messaging Test
//
// This test validates the locality-based message routing functionality:
// - local() / emit_local: Messages reach local recipients only (same process)
// - remote() / emit_remote: Messages reach remote recipients only (other processes)
// - world() / emit_global: Messages reach all recipients (local + remote)
//
// Test structure:
// - Coordinator process: Sends messages using all locality variants
// - Child process: Receives messages and reports what it received
//
// The test verifies that messages are routed according to their locality setting
// by having slots in both processes count messages and comparing against expectations.
//

#include <sintra/sintra.h>

#include "test_environment.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// Message types for the Maildrop API tests (uses Enclosure<T>)
struct LocalMsg { int value; };
struct RemoteMsg { int value; };
struct WorldMsg { int value; };

// Transceiver with typed messages for emit_* API tests
struct TestTransceiver : sintra::Derived_transceiver<TestTransceiver>
{
    TestTransceiver(const std::string& name = "") : sintra::Derived_transceiver<TestTransceiver>(name) {}

    SINTRA_MESSAGE(TypedLocalMsg, int value);
    SINTRA_MESSAGE(TypedRemoteMsg, int value);
    SINTRA_MESSAGE(TypedGlobalMsg, int value);

    void send_typed_local(int value)
    {
        emit_local<TypedLocalMsg>(value);
    }

    void send_typed_remote(int value)
    {
        emit_remote<TypedRemoteMsg>(value);
    }

    void send_typed_global(int value)
    {
        emit_global<TypedGlobalMsg>(value);
    }
};

constexpr std::string_view k_env_shared_dir = "SINTRA_LOCALITY_TEST_DIR";
constexpr std::string_view k_env_trace = "SINTRA_LOCALITY_TEST_TRACE";
constexpr int k_num_messages = 5;

bool trace_enabled()
{
    const char* value = std::getenv(k_env_trace.data());
    return value && *value && *value != '0';
}

void trace(const char* label)
{
    if (!trace_enabled()) {
        return;
    }
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    std::fprintf(stderr, "[locality_test][pid=%d] %s\n", pid, label);
    std::fflush(stderr);
}

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(k_env_shared_dir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_LOCALITY_TEST_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(k_env_shared_dir.data(), dir.string().c_str());
#else
    setenv(k_env_shared_dir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(k_env_shared_dir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("locality_test");
    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1);

    std::ostringstream oss;
    oss << "locality_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_counts(const std::filesystem::path& file,
                  int local_count, int remote_count, int world_count,
                  int typed_local_count, int typed_remote_count, int typed_global_count)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    out << local_count << '\n'
        << remote_count << '\n'
        << world_count << '\n'
        << typed_local_count << '\n'
        << typed_remote_count << '\n'
        << typed_global_count << '\n';
}

bool read_counts(const std::filesystem::path& file,
                 int& local_count, int& remote_count, int& world_count,
                 int& typed_local_count, int& typed_remote_count, int& typed_global_count)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    in >> local_count >> remote_count >> world_count
       >> typed_local_count >> typed_remote_count >> typed_global_count;
    return !in.fail();
}

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

// Counters for coordinator process (local slots)
std::atomic<int> g_coord_local_count{0};
std::atomic<int> g_coord_remote_count{0};
std::atomic<int> g_coord_world_count{0};
std::atomic<int> g_coord_typed_local_count{0};
std::atomic<int> g_coord_typed_remote_count{0};
std::atomic<int> g_coord_typed_global_count{0};

// Counters for child process
std::atomic<int> g_child_local_count{0};
std::atomic<int> g_child_remote_count{0};
std::atomic<int> g_child_world_count{0};
std::atomic<int> g_child_typed_local_count{0};
std::atomic<int> g_child_typed_remote_count{0};
std::atomic<int> g_child_typed_global_count{0};


int child_process()
{
    // Set up slots for Maildrop API messages
    sintra::activate_slot([](const LocalMsg& msg) {
        (void)msg;
        g_child_local_count.fetch_add(1, std::memory_order_relaxed);
    });

    sintra::activate_slot([](const RemoteMsg& msg) {
        (void)msg;
        g_child_remote_count.fetch_add(1, std::memory_order_relaxed);
    });

    sintra::activate_slot([](const WorldMsg& msg) {
        (void)msg;
        g_child_world_count.fetch_add(1, std::memory_order_relaxed);
    });

    // Set up slots for emit_* API messages (using the typed messages from TestTransceiver)
    sintra::activate_slot([](const TestTransceiver::TypedLocalMsg& msg) {
        (void)msg;
        g_child_typed_local_count.fetch_add(1, std::memory_order_relaxed);
    });

    sintra::activate_slot([](const TestTransceiver::TypedRemoteMsg& msg) {
        (void)msg;
        g_child_typed_remote_count.fetch_add(1, std::memory_order_relaxed);
    });

    sintra::activate_slot([](const TestTransceiver::TypedGlobalMsg& msg) {
        (void)msg;
        g_child_typed_global_count.fetch_add(1, std::memory_order_relaxed);
    });

    trace("child: slots-ready barrier enter");
    sintra::barrier("slots-ready", "_sintra_all_processes");
    trace("child: slots-ready barrier exit");

    // Wait for sender to finish
    trace("child: sending-done barrier enter");
    sintra::barrier("sending-done", "_sintra_all_processes");
    trace("child: sending-done barrier exit");

    // Small delay to ensure all messages are processed
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Write results to shared file
    const auto shared_dir = get_shared_directory();
    write_counts(shared_dir / "child_counts.txt",
                 g_child_local_count.load(),
                 g_child_remote_count.load(),
                 g_child_world_count.load(),
                 g_child_typed_local_count.load(),
                 g_child_typed_remote_count.load(),
                 g_child_typed_global_count.load());

    trace("child: results-written barrier enter");
    sintra::barrier("results-written", "_sintra_all_processes");
    trace("child: results-written barrier exit");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(child_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        // Coordinator process - set up local slots
        sintra::activate_slot([](const LocalMsg& msg) {
            (void)msg;
            g_coord_local_count.fetch_add(1, std::memory_order_relaxed);
        });

        sintra::activate_slot([](const RemoteMsg& msg) {
            (void)msg;
            g_coord_remote_count.fetch_add(1, std::memory_order_relaxed);
        });

        sintra::activate_slot([](const WorldMsg& msg) {
            (void)msg;
            g_coord_world_count.fetch_add(1, std::memory_order_relaxed);
        });

        sintra::activate_slot([](const TestTransceiver::TypedLocalMsg& msg) {
            (void)msg;
            g_coord_typed_local_count.fetch_add(1, std::memory_order_relaxed);
        });

        sintra::activate_slot([](const TestTransceiver::TypedRemoteMsg& msg) {
            (void)msg;
            g_coord_typed_remote_count.fetch_add(1, std::memory_order_relaxed);
        });

        sintra::activate_slot([](const TestTransceiver::TypedGlobalMsg& msg) {
            (void)msg;
            g_coord_typed_global_count.fetch_add(1, std::memory_order_relaxed);
        });

        trace("coord: slots-ready barrier enter");
        sintra::barrier("slots-ready", "_sintra_all_processes");
        trace("coord: slots-ready barrier exit");

        // Create a transceiver for emit_* tests
        TestTransceiver test_transceiver("test_sender");

        // Send messages using Maildrop API
        for (int i = 0; i < k_num_messages; ++i) {
            sintra::local() << LocalMsg{i};    // Should only reach coordinator
            sintra::remote() << RemoteMsg{i};  // Should only reach child
            sintra::world() << WorldMsg{i};    // Should reach both
        }

        // Send messages using emit_* API
        for (int i = 0; i < k_num_messages; ++i) {
            test_transceiver.send_typed_local(i);   // Should only reach coordinator
            test_transceiver.send_typed_remote(i);  // Should only reach child
            test_transceiver.send_typed_global(i);  // Should reach both
        }

        // Small delay to ensure messages are processed
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        trace("coord: sending-done barrier enter");
        sintra::barrier("sending-done", "_sintra_all_processes");
        trace("coord: sending-done barrier exit");

        // Write coordinator results
        write_counts(shared_dir / "coord_counts.txt",
                     g_coord_local_count.load(),
                     g_coord_remote_count.load(),
                     g_coord_world_count.load(),
                     g_coord_typed_local_count.load(),
                     g_coord_typed_remote_count.load(),
                     g_coord_typed_global_count.load());

        trace("coord: results-written barrier enter");
        sintra::barrier("results-written", "_sintra_all_processes");
        trace("coord: results-written barrier exit");
    }

    sintra::finalize();

    // Validation (coordinator only)
    if (!is_spawned) {
        int coord_local, coord_remote, coord_world;
        int coord_typed_local, coord_typed_remote, coord_typed_global;
        int child_local, child_remote, child_world;
        int child_typed_local, child_typed_remote, child_typed_global;

        bool read_ok = read_counts(shared_dir / "coord_counts.txt",
                                   coord_local, coord_remote, coord_world,
                                   coord_typed_local, coord_typed_remote, coord_typed_global);
        read_ok = read_ok && read_counts(shared_dir / "child_counts.txt",
                                          child_local, child_remote, child_world,
                                          child_typed_local, child_typed_remote, child_typed_global);

        if (!read_ok) {
            std::fprintf(stderr, "FAIL: Could not read result files\n");
            return 1;
        }

        bool passed = true;

        // Maildrop API tests
        // local() should only reach coordinator
        if (coord_local != k_num_messages) {
            std::fprintf(stderr, "FAIL: local() coordinator expected %d, got %d\n",
                         k_num_messages, coord_local);
            passed = false;
        }
        if (child_local != 0) {
            std::fprintf(stderr, "FAIL: local() child expected 0, got %d\n", child_local);
            passed = false;
        }

        // remote() should only reach child
        if (coord_remote != 0) {
            std::fprintf(stderr, "FAIL: remote() coordinator expected 0, got %d\n", coord_remote);
            passed = false;
        }
        if (child_remote != k_num_messages) {
            std::fprintf(stderr, "FAIL: remote() child expected %d, got %d\n",
                         k_num_messages, child_remote);
            passed = false;
        }

        // world() should reach both
        if (coord_world != k_num_messages) {
            std::fprintf(stderr, "FAIL: world() coordinator expected %d, got %d\n",
                         k_num_messages, coord_world);
            passed = false;
        }
        if (child_world != k_num_messages) {
            std::fprintf(stderr, "FAIL: world() child expected %d, got %d\n",
                         k_num_messages, child_world);
            passed = false;
        }

        // emit_* API tests
        // emit_local should only reach coordinator
        if (coord_typed_local != k_num_messages) {
            std::fprintf(stderr, "FAIL: emit_local() coordinator expected %d, got %d\n",
                         k_num_messages, coord_typed_local);
            passed = false;
        }
        if (child_typed_local != 0) {
            std::fprintf(stderr, "FAIL: emit_local() child expected 0, got %d\n", child_typed_local);
            passed = false;
        }

        // emit_remote should only reach child
        if (coord_typed_remote != 0) {
            std::fprintf(stderr, "FAIL: emit_remote() coordinator expected 0, got %d\n", coord_typed_remote);
            passed = false;
        }
        if (child_typed_remote != k_num_messages) {
            std::fprintf(stderr, "FAIL: emit_remote() child expected %d, got %d\n",
                         k_num_messages, child_typed_remote);
            passed = false;
        }

        // emit_global should reach both
        if (coord_typed_global != k_num_messages) {
            std::fprintf(stderr, "FAIL: emit_global() coordinator expected %d, got %d\n",
                         k_num_messages, coord_typed_global);
            passed = false;
        }
        if (child_typed_global != k_num_messages) {
            std::fprintf(stderr, "FAIL: emit_global() child expected %d, got %d\n",
                         k_num_messages, child_typed_global);
            passed = false;
        }

        // Cleanup
        bool cleanup_succeeded = false;
        for (int retry = 0; retry < 3 && !cleanup_succeeded; ++retry) {
            try {
                std::filesystem::remove_all(shared_dir);
                cleanup_succeeded = true;
            }
            catch (const std::exception& e) {
                if (retry < 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                else {
                    std::fprintf(stderr,
                                 "Warning: failed to remove temp directory %s after 3 attempts: %s\n",
                                 shared_dir.string().c_str(), e.what());
                }
            }
        }

        if (passed) {
            std::fprintf(stderr, "locality_messaging_test PASSED\n");
            return 0;
        }
        else {
            std::fprintf(stderr, "locality_messaging_test FAILED\n");
            return 1;
        }
    }

    return 0;
}

