#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

constexpr sintra::type_id_type k_collision_type_id = 0x7a31;

constexpr const char* k_child_flag    = "--targeted-process-child=";
constexpr const char* k_sender_flag   = "--targeted-process-sender=";
constexpr const char* k_rejected_flag = "--targeted-process-rejected=";

constexpr const char* k_service_a_name = "targeted_process_service_a";
constexpr const char* k_control_a_name = "targeted_process_control_a";
constexpr const char* k_service_b_name = "targeted_process_service_b";
constexpr const char* k_control_b_name = "targeted_process_control_b";

enum Probe_field
{
    exact_sender_count,
    global_count,
    process_sender_count,
    rejected_sender_count,
    event_value,
    unicast_count,
    unicast_value
};

struct Emitter : sintra::Derived_transceiver<Emitter>
{
    SINTRA_MESSAGE_EXPLICIT(process_event, k_collision_type_id, int value)
};

struct Probe_state
{
    std::atomic_int            exact_count{0};
    std::atomic_int            global_count{0};
    std::atomic_int            process_count{0};
    std::atomic_int            rejected_count{0};
    std::atomic_int            last_event_value{0};
    std::atomic_int            rpc_count{0};
    std::atomic_int            last_rpc_value{0};

    std::mutex                 mutex;
    std::condition_variable    changed;
    bool                       stop = false;
};

struct Collision_service : sintra::Derived_transceiver<Collision_service>
{
    explicit Collision_service(Probe_state& state)
        : m_state(state)
    {}

    void collide(int value)
    {
        m_state.last_rpc_value.store(value);
        m_state.rpc_count.fetch_add(1);
        m_state.changed.notify_all();
    }
    SINTRA_RPC_IMPL(
        collide,
        &Transceiver_type::collide,
        sintra::make_user_type_id(k_collision_type_id),
        true,
        true)

private:
    Probe_state& m_state;
};

using Collision_rpc_message = typename sintra::Transceiver::rpc_dispatcher<
    Collision_service::collide_mftc,
    decltype(&Collision_service::collide)>::message_type;

static_assert(
    Emitter::process_event::sintra_type_id() == Collision_rpc_message::sintra_type_id());

struct Probe_control : sintra::Derived_transceiver<Probe_control>
{
    explicit Probe_control(Probe_state& state)
        : m_state(state)
    {}

    int query(int field) const
    {
        switch (field) {
            case exact_sender_count:    return m_state.exact_count.load();
            case global_count:          return m_state.global_count.load();
            case process_sender_count:  return m_state.process_count.load();
            case rejected_sender_count: return m_state.rejected_count.load();
            case event_value:           return m_state.last_event_value.load();
            case unicast_count:         return m_state.rpc_count.load();
            case unicast_value:         return m_state.last_rpc_value.load();
            default:                    return -1;
        }
    }
    SINTRA_RPC(query)

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.stop = true;
        m_state.changed.notify_all();
    }
    SINTRA_UNICAST(stop)

private:
    Probe_state& m_state;
};

struct Child_handle
{
    sintra::Managed_child_custody  custody;
    sintra::instance_id_type       process_iid = sintra::invalid_instance_id;
    sintra::instance_id_type       service_iid = sintra::invalid_instance_id;
    sintra::instance_id_type       control_iid = sintra::invalid_instance_id;
};

std::string argument_value(int argc, char* argv[], std::string_view prefix)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.starts_with(prefix)) {
            return std::string(argument.substr(prefix.size()));
        }
    }
    return {};
}

sintra::instance_id_type parse_iid(const std::string& value)
{
    return static_cast<sintra::instance_id_type>(std::strtoull(value.c_str(), nullptr, 10));
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

void activate_probes(
    Probe_state&               state,
    sintra::instance_id_type   sender_iid,
    sintra::instance_id_type   rejected_iid)
{
    sintra::activate_slot(
        [&state](const Emitter::process_event& message) {
            state.last_event_value.store(message.value);
            state.exact_count.fetch_add(1);
            state.changed.notify_all();
        },
        sintra::Typed_instance_id<Emitter>(sender_iid));

    sintra::activate_slot(
        [&state](const Emitter::process_event& message) {
            state.last_event_value.store(message.value);
            state.global_count.fetch_add(1);
            state.changed.notify_all();
        });

    sintra::activate_slot(
        [&state](const Emitter::process_event&) {
            state.process_count.fetch_add(1);
            state.changed.notify_all();
        },
        sintra::Typed_instance_id<void>(sintra::process_of(sender_iid)));

    sintra::activate_slot(
        [&state](const Emitter::process_event&) {
            state.rejected_count.fetch_add(1);
            state.changed.notify_all();
        },
        sintra::Typed_instance_id<Emitter>(rejected_iid));
}

bool wait_for_event(Probe_state& state, int expected)
{
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.changed.wait_for(lock, 10s, [&]() {
        return
            state.exact_count.load()  == expected &&
            state.global_count.load() == expected;
    });
}

int run_child(
    const std::string&         role,
    sintra::instance_id_type   sender_iid,
    sintra::instance_id_type   rejected_iid)
{
    const bool is_process_a = role == "a";
    if (!is_process_a && role != "b") {
        return 1;
    }

    const char* service_name = is_process_a ? k_service_a_name : k_service_b_name;
    const char* control_name = is_process_a ? k_control_a_name : k_control_b_name;

    Probe_state       state;
    Collision_service service(state);
    Probe_control control(state);
    activate_probes(state, sender_iid, rejected_iid);

    if (!service.assign_name(service_name) || !control.assign_name(control_name)) {
        return 1;
    }

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool stopped = state.changed.wait_for(lock, 30s, [&]() {
        return state.stop;
    });
    lock.unlock();

    sintra::deactivate_all_slots();
    return stopped ? 0 : 1;
}

bool spawn_child(
    const std::string&         binary_path,
    const char*                role,
    sintra::instance_id_type   sender_iid,
    sintra::instance_id_type   rejected_iid,
    Child_handle&              child)
{
    const bool  is_process_a = std::string_view(role) == "a";
    const char* service_name = is_process_a ? k_service_a_name : k_service_b_name;
    const char* control_name = is_process_a ? k_control_a_name : k_control_b_name;

    sintra::Spawn_options options;
    options.binary_path             = binary_path;
    options.args                    = {
        std::string(k_child_flag) + role,
        std::string(k_sender_flag) + std::to_string(sender_iid),
        std::string(k_rejected_flag) + std::to_string(rejected_iid)};
    options.readiness_instance_name = control_name;

    child.custody = sintra::spawn_swarm_process(options);
    const auto readiness = child.custody.wait_for_readiness_until(
        std::chrono::steady_clock::now() + 15s);
    if (!child.custody ||
        readiness.readiness_state != sintra::Managed_child_readiness_state::reached)
    {
        return false;
    }

    child.service_iid = sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id,
        service_name);
    child.control_iid = sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id,
        control_name);
    child.process_iid = sintra::process_of(child.control_iid);
    return
        child.service_iid != sintra::invalid_instance_id &&
        child.control_iid != sintra::invalid_instance_id &&
        sintra::detail::is_valid_process_instance_id(child.process_iid);
}

bool query_equals(
    sintra::instance_id_type   control_iid,
    Probe_field                field,
    int                        expected,
    const char*                message)
{
    try {
        return expect(Probe_control::rpc_query(control_iid, field) == expected, message);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s: %s\n", message, error.what());
        return false;
    }
}

bool stop_child(Child_handle& child)
{
    if (!child.custody) {
        return true;
    }

    if (child.control_iid != sintra::invalid_instance_id) {
        try {
            Probe_control::rpc_stop(child.control_iid);
        }
        catch (...) {
        }
    }

    const auto status = child.custody.release_until(
        std::chrono::steady_clock::now() + 15s);
    return status.release_state == sintra::Managed_child_release_state::complete;
}

bool expect_invalid_process(
    Emitter&                   emitter,
    sintra::instance_id_type   process_iid,
    const char*                message)
{
    const auto start = std::chrono::steady_clock::now();
    bool       threw = false;
    try {
        emitter.emit_to_process<Emitter::process_event>(process_iid, 41);
    }
    catch (const std::invalid_argument&) {
        threw = true;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return
        expect(threw, message) &&
        expect(elapsed < 500ms, "invalid process rejection should be bounded");
}

bool expect_unicast_unavailable(
    sintra::instance_id_type   service_iid,
    const char*                message)
{
    bool threw = false;
    try {
        Collision_service::rpc_collide(service_iid, 52);
    }
    catch (const sintra::rpc_unavailable&) {
        threw = true;
    }
    return expect(threw, message);
}

bool run_local_contract(Emitter& emitter, Emitter& rejected)
{
    bool              ok = true;
    Probe_state       state;
    Collision_service service(state);
    activate_probes(state, emitter.instance_id(), rejected.instance_id());

    emitter.emit_to_process<Emitter::process_event>(sintra::s_mproc_id, 11);
    ok &= expect(wait_for_event(state, 1), "local process slots should receive exactly once");
    ok &= expect(state.process_count.load() == 0, "local process sender filter should not match");
    ok &= expect(state.rejected_count.load() == 0, "co-resident rejected sender should not match");
    ok &= expect(state.rpc_count.load() == 0, "process event collision should not invoke unicast");
    ok &= expect(state.last_event_value.load() == 11, "local process event payload should arrive");

    Collision_service::rpc_collide(service.instance_id(), 21);
    ok &= expect(state.rpc_count.load() == 1, "live local SINTRA_UNICAST should invoke once");
    ok &= expect(state.last_rpc_value.load() == 21, "live local SINTRA_UNICAST should carry payload");
    ok &= expect(state.exact_count.load() == 1, "SINTRA_UNICAST should not invoke event slot");
    ok &= expect(state.global_count.load() == 1, "SINTRA_UNICAST should not invoke global slot");

    ok &= expect_invalid_process(
        emitter,
        sintra::invalid_instance_id,
        "invalid IID should be rejected as a process target");
    ok &= expect_invalid_process(
        emitter,
        emitter.instance_id(),
        "endpoint IID should be rejected as a process target");

    const auto missing_iid = sintra::make_instance_id();
    ok &= expect_unicast_unavailable(
        missing_iid,
        "missing local SINTRA_UNICAST target should stay unavailable");

    sintra::instance_id_type departed_iid = sintra::invalid_instance_id;
    {
        Collision_service departed(state);
        departed_iid = departed.instance_id();
    }
    ok &= expect_unicast_unavailable(
        departed_iid,
        "departed local SINTRA_UNICAST target should stay unavailable");
    ok &= expect(state.rpc_count.load() == 1, "missing/departed unicast should not invoke handler");
    return ok;
}

bool run_remote_contract(
    const std::string& binary_path,
    Emitter&           emitter,
    Emitter&           rejected)
{
    bool         ok = true;
    Child_handle process_a;
    Child_handle process_b;

    if (!spawn_child(binary_path, "a", emitter.instance_id(), rejected.instance_id(), process_a) ||
        !spawn_child(binary_path, "b", emitter.instance_id(), rejected.instance_id(), process_b))
    {
        std::fprintf(stderr, "FAIL: spawned process observers should become ready\n");
        stop_child(process_a);
        stop_child(process_b);
        return false;
    }

    emitter.emit_to_process<Emitter::process_event>(process_a.process_iid, 61);
    ok &= query_equals(
        process_a.control_iid,
        exact_sender_count,
        1,
        "remote exact sender slot should receive exactly once");
    ok &= query_equals(
        process_a.control_iid,
        global_count,
        1,
        "remote process-global slot should receive exactly once");
    ok &= query_equals(
        process_a.control_iid,
        process_sender_count,
        0,
        "remote process sender filter should not match endpoint sender");
    ok &= query_equals(
        process_a.control_iid,
        rejected_sender_count,
        0,
        "remote co-resident rejected sender should not match");
    ok &= query_equals(
        process_a.control_iid,
        unicast_count,
        0,
        "colliding process event should not invoke RPC/unicast handler");
    ok &= query_equals(
        process_a.control_iid,
        event_value,
        61,
        "remote process event should carry payload");

    ok &= query_equals(
        process_b.control_iid,
        exact_sender_count,
        0,
        "other process exact slot should not observe message");
    ok &= query_equals(
        process_b.control_iid,
        global_count,
        0,
        "other process global slot should not observe message");
    ok &= query_equals(
        process_b.control_iid,
        process_sender_count,
        0,
        "other process sender filter should not observe message");

    Collision_service::rpc_collide(process_a.service_iid, 71);
    ok &= query_equals(
        process_a.control_iid,
        unicast_count,
        1,
        "live remote SINTRA_UNICAST should invoke handler once");
    ok &= query_equals(
        process_a.control_iid,
        unicast_value,
        71,
        "live remote SINTRA_UNICAST should carry payload");
    ok &= query_equals(
        process_a.control_iid,
        exact_sender_count,
        1,
        "colliding SINTRA_UNICAST should not invoke event slot");
    ok &= query_equals(
        process_a.control_iid,
        global_count,
        1,
        "colliding SINTRA_UNICAST should not invoke global slot");

    const auto missing_remote_iid = sintra::compose_instance(
        static_cast<uint32_t>(sintra::get_process_index(process_b.process_iid)),
        sintra::max_instance_index - 1);
    const auto missing_start = std::chrono::steady_clock::now();
    Collision_service::rpc_collide(missing_remote_iid, 81);
    const auto missing_elapsed = std::chrono::steady_clock::now() - missing_start;
    ok &= expect(missing_elapsed < 500ms, "missing remote SINTRA_UNICAST should be bounded");
    ok &= query_equals(
        process_b.control_iid,
        unicast_count,
        0,
        "missing remote SINTRA_UNICAST should not invoke handler");

    const auto departed_process_iid = process_a.process_iid;
    const auto departed_service_iid = process_a.service_iid;
    ok &= expect(stop_child(process_a), "process A should stop cleanly");

    const auto departed_event_start = std::chrono::steady_clock::now();
    emitter.emit_to_process<Emitter::process_event>(departed_process_iid, 91);
    const auto departed_event_elapsed =
        std::chrono::steady_clock::now() - departed_event_start;
    ok &= expect(departed_event_elapsed < 500ms, "departed process event should be bounded");

    const auto departed_unicast_start = std::chrono::steady_clock::now();
    Collision_service::rpc_collide(departed_service_iid, 92);
    const auto departed_unicast_elapsed =
        std::chrono::steady_clock::now() - departed_unicast_start;
    ok &= expect(
        departed_unicast_elapsed < 500ms,
        "departed remote SINTRA_UNICAST should be bounded");
    ok &= query_equals(
        process_b.control_iid,
        exact_sender_count,
        0,
        "runtime should remain responsive after departed sends");

    ok &= expect(stop_child(process_b), "process B should stop cleanly");
    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const std::string binary_path  = sintra::test::get_binary_path(argc, argv);
    const std::string child_role   = argument_value(argc, argv, k_child_flag);
    const auto        sender_iid   = parse_iid(argument_value(argc, argv, k_sender_flag));
    const auto        rejected_iid = parse_iid(argument_value(argc, argv, k_rejected_flag));

    sintra::init(argc, argv);

    int result = 0;
    if (!child_role.empty()) {
        result = run_child(child_role, sender_iid, rejected_iid);
    }
    else {
        Emitter emitter;
        Emitter rejected;
        const bool local_ok  = run_local_contract(emitter, rejected);
        const bool remote_ok = run_remote_contract(binary_path, emitter, rejected);
        result = local_ok && remote_ok ? 0 : 1;
    }

    sintra::detail::finalize();
    return result;
}
