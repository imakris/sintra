//
// Sintra Parent->Child Targeted RPC Delivery Test
//
// Regression test for the basic delivery contract: once a spawned child
// publishes a named transceiver and the parent resolves it, parent -> child
// targeted commands must be delivered.  Verifies both blocking RPC (SINTRA_RPC)
// and fire-and-forget unicast (SINTRA_UNICAST) targeting an instance that
// lives in another process.
//
// The test answers exactly:
//   "After a spawned child publishes a named transceiver and the parent
//    resolves that name, can the parent deliver both a blocking targeted RPC
//    and a targeted fire-and-forget command to that child transceiver, without
//    deadlock, and can the child report command execution back through Sintra?"
//
// Both directions (parent -> child and child -> parent) are exercised through
// named transceivers and targeted RPCs.  Specifically:
//
//   Parent_ack    : sintra::Derived_transceiver owned by the parent.  Hosts a
//                   SINTRA_UNICAST mark_seen(int) handler that captures the
//                   value reported by the child.  Published under the well
//                   known name k_parent_ack_name.
//
//   Child_service : sintra::Derived_transceiver owned by the child.  Hosts a
//                   SINTRA_RPC ping(int) and a SINTRA_UNICAST mark(int).  The
//                   child's mark() handler resolves k_parent_ack_name via the
//                   coordinator and invokes Parent_ack::rpc_mark_seen on the
//                   parent.  This proves the parent's mark() command actually
//                   ran in the child by carrying the proof back over Sintra
//                   instead of through a temp-file side channel.
//
// Scenario:
//   1. Parent (coordinator) initializes Sintra and constructs a Parent_ack
//      receiver, publishing it under k_parent_ack_name so the child can
//      resolve it.
//   2. Parent calls spawn_swarm_process with wait_for_instance_name =
//      k_child_service_name so the spawned child has published its named
//      transceiver before the parent proceeds.
//   3. Parent resolves k_child_service_name -> child_iid via the coordinator
//      and asserts:
//        - child_iid is valid;
//        - child_iid lives in a different process than the parent;
//        - child_iid is NOT the child's Managed_process instance id (the name
//          must resolve to the named Child_service transceiver, not to the
//          process-level transceiver).
//   4. Parent calls Child_service::rpc_ping(child_iid, ...) (blocking RPC)
//      and asserts the reply was produced by the child handler.
//   5. Parent calls Child_service::rpc_mark(child_iid, ...) (SINTRA_UNICAST,
//      fire-and-forget).  This call must return without waiting for a remote
//      round-trip.
//   6. Child's mark() handler invokes Parent_ack::rpc_mark_seen back on the
//      parent.  Parent waits (with a bounded timeout) for the recorded value
//      to match.
//   7. Parent emits Done_signal so the child exits cleanly, then both
//      processes finalize.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using sintra::s_coord_id;
using sintra::s_mproc_id;

namespace {

constexpr const char* k_child_service_name = "child_service";
constexpr const char* k_parent_ack_name    = "parent_ack";

constexpr int k_ping_value = 123;
constexpr int k_mark_value = 456;

// Done_signal is broadcast by the parent to ask the child to exit (matches the
// helper used in tests/spawn_wait_test.cpp).
struct Done_signal {};

// Parent-side state populated by Parent_ack::mark_seen so the coordinator can
// wait for the ack with a bounded timeout instead of polling a side channel.
struct Ack_state
{
    std::mutex mutex;
    std::condition_variable cv;
    bool seen  = false;
    int  value = 0;
};

inline Ack_state& parent_ack_state()
{
    static Ack_state s;
    return s;
}

// Receiver-side transceiver owned by the parent.  Published under
// k_parent_ack_name so the child can resolve it and target a unicast back.
struct Parent_ack : sintra::Derived_transceiver<Parent_ack>
{
    void mark_seen(int value)
    {
        std::fprintf(stderr, "[COORD] Parent_ack::mark_seen(%d) handler invoked\n", value);
        Ack_state& st = parent_ack_state();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.value = value;
        st.seen  = true;
        st.cv.notify_all();
    }
    SINTRA_UNICAST(mark_seen)
};

// Child-side cache for the resolved Parent_ack iid so the mark() handler
// performs name resolution at most once.
inline std::atomic<sintra::instance_id_type>& cached_parent_ack_iid()
{
    static std::atomic<sintra::instance_id_type> cached{sintra::invalid_instance_id};
    return cached;
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

    // Reports execution back to the parent via a targeted Sintra unicast on
    // the named Parent_ack transceiver.  This proves the parent -> child
    // command actually ran in the child by routing the proof through Sintra
    // (instead of a temp file or other side channel) and exercises the
    // child -> parent direction with real targeted messaging.
    void mark(int value)
    {
        std::fprintf(stderr, "[CHILD] mark(%d) handler invoked\n", value);

        sintra::instance_id_type ack_iid = cached_parent_ack_iid().load();
        if (ack_iid == sintra::invalid_instance_id) {
            ack_iid = sintra::Coordinator::rpc_resolve_instance(
                s_coord_id, k_parent_ack_name);
            if (ack_iid != sintra::invalid_instance_id) {
                cached_parent_ack_iid().store(ack_iid);
            }
        }

        if (ack_iid == sintra::invalid_instance_id) {
            std::fprintf(stderr,
                         "[CHILD] mark(): could not resolve '%s' to ack the parent\n",
                         k_parent_ack_name);
            return;
        }

        Parent_ack::rpc_mark_seen(ack_iid, value);
    }
    SINTRA_UNICAST(mark)
};

bool is_child_mode(int argc, char* argv[])
{
    // Sintra adds --instance_id to argv when this process was spawned by
    // spawn_swarm_process. The coordinator never gets that flag.  This matches
    // the convention used in tests/spawn_wait_test.cpp.
    return sintra::test::has_argv_flag(argc, argv, "--instance_id");
}

int run_child()
{
    std::fprintf(stderr, "[CHILD] starting\n");

    Child_service service;
    if (!service.assign_name(k_child_service_name)) {
        std::fprintf(stderr, "[CHILD] failed to assign name '%s'\n", k_child_service_name);
        return 1;
    }
    std::fprintf(stderr,
                 "[CHILD] published '%s' iid=%llu (mproc_iid=%llu)\n",
                 k_child_service_name,
                 static_cast<unsigned long long>(service.instance_id()),
                 static_cast<unsigned long long>(s_mproc_id));

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

int run_coordinator(const std::string& binary_path)
{
    std::fprintf(stderr, "[COORD] starting (mproc_id=%llu, coord_id=%llu)\n",
                 static_cast<unsigned long long>(s_mproc_id),
                 static_cast<unsigned long long>(s_coord_id));

    // Construct and publish the parent's ack receiver before spawning so the
    // child can resolve it as soon as it needs to ack.
    Parent_ack ack_receiver;
    if (!ack_receiver.assign_name(k_parent_ack_name)) {
        std::fprintf(stderr, "[COORD] failed to assign name '%s'\n", k_parent_ack_name);
        return 1;
    }
    std::fprintf(stderr,
                 "[COORD] published '%s' iid=%llu\n",
                 k_parent_ack_name,
                 static_cast<unsigned long long>(ack_receiver.instance_id()));

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
    const auto child_proc_iid = sintra::process_of(child_iid);
    std::fprintf(stderr,
                 "[COORD] resolved '%s' -> %llu (process_iid=%llu)\n",
                 k_child_service_name,
                 static_cast<unsigned long long>(child_iid),
                 static_cast<unsigned long long>(child_proc_iid));

    auto teardown_and_return = [&](int rv) {
        sintra::world() << Done_signal{};
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return rv;
    };

    if (!sintra::test::assert_true(child_iid != sintra::invalid_instance_id,
                                   "[COORD] ",
                                   "resolved child_iid must be valid"))
    {
        return teardown_and_return(1);
    }
    if (!sintra::test::assert_true(child_proc_iid != sintra::process_of(s_mproc_id),
                                   "[COORD] ",
                                   "resolved child_iid must belong to a different process than the parent"))
    {
        return teardown_and_return(1);
    }
    // Negative/precision assertion: the resolved id must be the named
    // Child_service transceiver, NOT the child's Managed_process instance.
    // process_of(iid) returns the Managed_process instance id for that
    // process; the named transceiver must have a different (non-process)
    // sub-id within the same process.
    if (!sintra::test::assert_true(child_iid != child_proc_iid,
                                   "[COORD] ",
                                   "resolved child_iid must be the named Child_service transceiver, "
                                   "not the child's Managed_process instance"))
    {
        return teardown_and_return(1);
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
        return teardown_and_return(1);
    }
    std::fprintf(stderr, "[COORD] rpc_ping returned %d\n", ping_reply);
    if (!sintra::test::assert_true(ping_reply == k_ping_value + 1,
                                   "[COORD] ",
                                   "rpc_ping reply must equal value+1 produced by the child handler"))
    {
        return teardown_and_return(1);
    }

    // ---- 4. fire-and-forget unicast to a specific remote instance ---------
    std::fprintf(stderr, "[COORD] calling Child_service::rpc_mark(%llu, %d)\n",
                 static_cast<unsigned long long>(child_iid), k_mark_value);
    const auto mark_start = std::chrono::steady_clock::now();
    try {
        Child_service::rpc_mark(child_iid, k_mark_value);
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "[COORD] rpc_mark threw: %s\n", ex.what());
        return teardown_and_return(1);
    }
    const auto mark_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - mark_start).count();
    // Fire-and-forget must not block on a remote round-trip.
    if (!sintra::test::assert_true(mark_elapsed < 500,
                                   "[COORD] ",
                                   "fire-and-forget rpc_mark must return quickly (no remote wait)"))
    {
        return teardown_and_return(1);
    }

    // ---- 5. wait for the child's targeted Sintra ack ----------------------
    bool got_ack = false;
    int  observed_value = 0;
    {
        Ack_state& st = parent_ack_state();
        std::unique_lock<std::mutex> lk(st.mutex);
        got_ack = st.cv.wait_for(lk, std::chrono::seconds(10), [&] { return st.seen; });
        observed_value = st.value;
    }

    bool ok = true;
    ok &= sintra::test::assert_true(got_ack,
                                    "[COORD] ",
                                    "child mark() must report execution back via Parent_ack::rpc_mark_seen");
    if (got_ack) {
        ok &= sintra::test::assert_true(observed_value == k_mark_value,
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

    const std::string binary_path = sintra::test::get_binary_path(argc, argv);

    if (is_child_mode(argc, argv)) {
        sintra::init(argc, argv);
        const int rv = run_child();
        sintra::detail::finalize();
        return rv;
    }

    sintra::init(argc, argv);
    const int rv = run_coordinator(binary_path);
    sintra::detail::finalize();

    return rv;
}
