//
// Sintra Parent->Child Targeted RPC Delivery Test
//
// Regression test for the basic delivery contract: once a spawned child
// publishes a named transceiver and the parent resolves it, parent -> child
// targeted commands must be delivered.  Verifies both blocking RPC (SINTRA_RPC)
// and fire-and-forget unicast (SINTRA_UNICAST) targeting an instance that
// lives in another process.
//
// Scenario:
//   1. Parent (coordinator) calls sintra::init() and spawn_swarm_process()
//      with wait_for_instance_name = "child_service" so the spawned child has
//      published its named transceiver before the parent proceeds.
//   2. Parent resolves "child_service" -> child_iid via the coordinator.
//   3. Parent invokes Child_service::rpc_ping(child_iid, ...) and expects
//      a reply produced by the child handler.
//   4. Parent invokes Child_service::rpc_mark(child_iid, ...) (SINTRA_UNICAST)
//      and waits for the child to acknowledge via a temp file that mark()
//      actually executed in the child process.
//   5. Parent signals the child to exit and shuts down.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using sintra::s_coord_id;
using sintra::s_mproc_id;

namespace {

constexpr const char* k_child_service_name = "child_service";
constexpr const char* k_ack_filename       = "child_mark_ack.txt";

constexpr int k_ping_value = 123;
constexpr int k_mark_value = 456;

struct Done_signal {};

// Storage for child-side state used by the Child_service handlers.
// A global keeps the message handler bodies short while letting the coordinator
// learn the path to the ack file via the shared scratch directory.
inline std::filesystem::path& ack_path()
{
    static std::filesystem::path path;
    return path;
}

// Child-side transceiver. ping() is a blocking RPC; mark() is fire-and-forget
// (SINTRA_UNICAST) targeted at this specific instance from another process.
struct Child_service : sintra::Derived_transceiver<Child_service>
{
    // Returns ping_value + 1 to demonstrate the child handler ran AND that
    // the reply payload is propagated back to the parent unchanged.
    int ping(int value)
    {
        std::fprintf(stderr, "[CHILD] ping(%d) handler invoked\n", value);
        return value + 1;
    }
    SINTRA_RPC(ping)

    // Writes the value to a known file so the parent can confirm execution.
    void mark(int value)
    {
        std::fprintf(stderr, "[CHILD] mark(%d) handler invoked\n", value);
        std::ofstream out(ack_path(), std::ios::binary | std::ios::trunc);
        out << value;
    }
    SINTRA_UNICAST(mark)
};

bool is_child_mode(int argc, char* argv[])
{
    // Sintra adds --instance_id to argv when this process was spawned by
    // spawn_swarm_process. The coordinator never gets that flag.
    return sintra::test::has_argv_flag(argc, argv, "--instance_id");
}

int run_child(const std::filesystem::path& shared_dir)
{
    std::fprintf(stderr, "[CHILD] starting\n");

    ack_path() = shared_dir / k_ack_filename;

    // Ensure no stale ack file exists.
    std::error_code ec;
    std::filesystem::remove(ack_path(), ec);

    Child_service service;
    if (!service.assign_name(k_child_service_name)) {
        std::fprintf(stderr, "[CHILD] failed to assign name '%s'\n", k_child_service_name);
        return 1;
    }
    std::fprintf(stderr,
                 "[CHILD] published '%s' iid=%llu\n",
                 k_child_service_name,
                 static_cast<unsigned long long>(service.instance_id()));

    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;

    sintra::activate_slot([&](const Done_signal&) {
        std::fprintf(stderr, "[CHILD] received Done_signal\n");
        std::lock_guard<std::mutex> lk(done_mutex);
        done = true;
        done_cv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lk(done_mutex);
        if (!done_cv.wait_for(lk, std::chrono::seconds(30), [&] { return done; })) {
            std::fprintf(stderr, "[CHILD] timed out waiting for Done_signal\n");
            sintra::deactivate_all_slots();
            return 1;
        }
    }

    sintra::deactivate_all_slots();
    std::fprintf(stderr, "[CHILD] exiting cleanly\n");
    return 0;
}

int run_coordinator(const std::string& binary_path, const std::filesystem::path& shared_dir)
{
    std::fprintf(stderr, "[COORD] starting (mproc_id=%llu, coord_id=%llu)\n",
                 static_cast<unsigned long long>(s_mproc_id),
                 static_cast<unsigned long long>(s_coord_id));

    // The same shared scratch dir is forwarded to the spawned child via the
    // SINTRA_TEST_SHARED_DIR environment variable (see Shared_directory).
    const auto ack_file = shared_dir / k_ack_filename;
    std::error_code ec;
    std::filesystem::remove(ack_file, ec);

    // ---- 1. spawn the child and wait for its named transceiver ------------
    sintra::Spawn_options spawn_options;
    spawn_options.binary_path           = binary_path;
    spawn_options.wait_for_instance_name = k_child_service_name;
    spawn_options.wait_timeout           = std::chrono::milliseconds(15000);

    const size_t spawned = sintra::spawn_swarm_process(spawn_options);
    if (!sintra::test::assert_true(spawned == 1,
                                   "[COORD] ",
                                   "spawn_swarm_process should report a single child"))
    {
        return 1;
    }

    // ---- 2. resolve the named transceiver ---------------------------------
    const auto child_iid = sintra::Coordinator::rpc_resolve_instance(
        s_coord_id, k_child_service_name);
    std::fprintf(stderr, "[COORD] resolved '%s' -> %llu\n",
                 k_child_service_name,
                 static_cast<unsigned long long>(child_iid));

    if (!sintra::test::assert_true(child_iid != sintra::invalid_instance_id,
                                   "[COORD] ",
                                   "resolved child_iid must be valid"))
    {
        sintra::world() << Done_signal{};
        return 1;
    }
    if (!sintra::test::assert_true(sintra::process_of(child_iid) != sintra::process_of(s_mproc_id),
                                   "[COORD] ",
                                   "resolved child_iid must belong to a different process than the parent"))
    {
        sintra::world() << Done_signal{};
        return 1;
    }

    // ---- 3. blocking RPC: parent calls child handler, expects reply -------
    std::fprintf(stderr, "[COORD] calling Child_service::rpc_ping(%llu, %d)\n",
                 static_cast<unsigned long long>(child_iid), k_ping_value);
    int ping_reply = 0;
    try {
        ping_reply = Child_service::rpc_ping(child_iid, k_ping_value);
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "[COORD] rpc_ping threw: %s\n", ex.what());
        sintra::world() << Done_signal{};
        return 1;
    }
    std::fprintf(stderr, "[COORD] rpc_ping returned %d\n", ping_reply);
    if (!sintra::test::assert_true(ping_reply == k_ping_value + 1,
                                   "[COORD] ",
                                   "rpc_ping reply must equal value+1 produced by the child handler"))
    {
        sintra::world() << Done_signal{};
        return 1;
    }

    // ---- 4. fire-and-forget unicast to a specific remote instance ---------
    std::fprintf(stderr, "[COORD] calling Child_service::rpc_mark(%llu, %d)\n",
                 static_cast<unsigned long long>(child_iid), k_mark_value);
    try {
        Child_service::rpc_mark(child_iid, k_mark_value);
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "[COORD] rpc_mark threw: %s\n", ex.what());
        sintra::world() << Done_signal{};
        return 1;
    }

    // ---- 5. wait for the child to write the ack file ----------------------
    bool ack_seen = false;
    int  ack_value = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(ack_file)) {
            std::ifstream in(ack_file, std::ios::binary);
            if (in >> ack_value) {
                ack_seen = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    bool ok = true;
    ok &= sintra::test::assert_true(ack_seen,
                                    "[COORD] ",
                                    "child mark() handler must run (ack file should appear)");
    if (ack_seen) {
        ok &= sintra::test::assert_true(ack_value == k_mark_value,
                                        "[COORD] ",
                                        "child mark() must observe the value sent by the parent");
    }

    sintra::world() << Done_signal{};

    // Best-effort: give the child a moment to drain the Done_signal so
    // shutdown does not race against an in-flight handler.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR", "parent_child_targeted_rpc_test");
    const auto shared_dir = shared.path();
    const std::string binary_path = sintra::test::get_binary_path(argc, argv);

    if (is_child_mode(argc, argv)) {
        sintra::init(argc, argv);
        const int rv = run_child(shared_dir);
        sintra::detail::finalize();
        return rv;
    }

    sintra::init(argc, argv);
    const int rv = run_coordinator(binary_path, shared_dir);
    sintra::detail::finalize();

    shared.cleanup();
    return rv;
}
