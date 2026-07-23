#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr const char* k_role_arg       = "--first-rpc-role";
constexpr const char* k_stage_arg      = "--first-rpc-stage";
constexpr const char* k_value_arg      = "--first-rpc-value";
constexpr const char* k_entered_arg    = "--first-rpc-entered";
constexpr const char* k_release_arg    = "--first-rpc-release";
constexpr const char* k_probe_name_arg = "--first-rpc-probe-name";
constexpr const char* k_victim_role    = "victim";
constexpr const char* k_leaver_role    = "leaver";
constexpr const char* k_service_name   = "external_first_rpc_service";
constexpr const char* k_probe_name     = "external_first_rpc_probe";
constexpr const char* k_failure_prefix =
    "external_process_first_rpc_concurrency_test: ";

struct Ping_service : sintra::Derived_transceiver<Ping_service>
{
    int ping(int value)
    {
        return value + 1;
    }

    SINTRA_RPC(ping)
};

struct Probe_service : sintra::Derived_transceiver<Probe_service>
{
    int probe(int value)
    {
        return value + 1;
    }

    SINTRA_RPC(probe)
};

using Ping_message = typename sintra::Transceiver::rpc_dispatcher<
    Ping_service::ping_mftc,
    decltype(&Ping_service::ping)>::message_type;

struct Type_id_gate
{
    bool                  enabled = false;
    std::filesystem::path entered;
    std::filesystem::path release;
};

Type_id_gate g_type_id_gate;

struct Blocking_ping_message : Ping_message
{
    explicit Blocking_ping_message(int value)
    :
        Ping_message(value)
    {}

    // Avoid Clang probing Message's forwarding constructor while synthesizing
    // a derived copy constructor and treating the message itself as an argument.
    Blocking_ping_message(const Blocking_ping_message& other)
    :
        Blocking_ping_message(
            sintra::detail::get<0>(
                static_cast<const sintra::detail::message_args<int>&>(other)))
    {}

    static sintra::type_id_type id()
    {
        if (g_type_id_gate.enabled) {
            sintra::test::write_lines(g_type_id_gate.entered, {"entered"});
            if (!sintra::test::wait_for_file(
                    g_type_id_gate.release,
                    10s,
                    1ms))
            {
                throw std::runtime_error(
                    "timed out waiting to release type resolution");
            }
        }
        return Ping_message::id();
    }
};

static_assert(sizeof(Blocking_ping_message) == sizeof(Ping_message));
static_assert(alignof(Blocking_ping_message) == alignof(Ping_message));

} // namespace

namespace sintra {

template <>
struct ring_payload_traits<::Blocking_ping_message>
{
    static constexpr bool allow_nontrivial = true;
};

} // namespace sintra

namespace {

struct Runtime_guard
{
    bool active = false;

    ~Runtime_guard()
    {
        if (active && sintra::s_mproc) {
            try {
                sintra::detail::finalize();
            }
            catch (...) {
            }
        }
    }

    bool shutdown()
    {
        if (!active) {
            return true;
        }
        active = false;
        return sintra::shutdown();
    }

    bool leave()
    {
        if (!active) {
            return true;
        }
        active = false;
        return sintra::leave();
    }
};

void write_stage(
    const std::filesystem::path& path,
    const std::string&           stage)
{
    sintra::test::write_lines(path, {stage});
}

std::string read_stage(const std::filesystem::path& path)
{
    const auto lines = sintra::test::read_lines(path);
    return lines.empty() ? "missing" : lines.front();
}

int run_child(int argc, char* argv[])
{
    const std::string role =
        sintra::test::get_argv_value(argc, argv, k_role_arg);
    const std::filesystem::path stage_path =
        sintra::test::get_argv_value(argc, argv, k_stage_arg);
    const std::filesystem::path entered_path =
        sintra::test::get_argv_value(argc, argv, k_entered_arg);
    const std::filesystem::path release_path =
        sintra::test::get_argv_value(argc, argv, k_release_arg);
    const std::string probe_name =
        sintra::test::get_argv_value(argc, argv, k_probe_name_arg);
    const int value = std::stoi(
        sintra::test::get_argv_value(argc, argv, k_value_arg));
    write_stage(stage_path, "starting");

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& e) {
        write_stage(stage_path, std::string("init_failed: ") + e.what());
        return 2;
    }
    catch (...) {
        write_stage(stage_path, "init_failed: unknown exception");
        return 2;
    }
    Runtime_guard guard{true};
    write_stage(stage_path, "initialized");

    const auto service = sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id,
        k_service_name);
    if (service == sintra::invalid_instance_id) {
        write_stage(stage_path, "resolve_failed");
        return guard.leave() ? 3 : 4;
    }

    std::unique_ptr<Probe_service> probe;
    if (role == k_victim_role) {
        probe = std::make_unique<Probe_service>();
        if (!probe->assign_name(probe_name)) {
            write_stage(stage_path, "probe_publish_failed");
            probe.reset();
            return guard.leave() ? 5 : 6;
        }
        g_type_id_gate = {true, entered_path, release_path};
        write_stage(stage_path, "type_gate_ready");
    }

    try {
        auto ping = role == k_victim_role
            ? sintra::Transceiver::rpc_async_impl<
                Ping_service::ping_mftc,
                Blocking_ping_message,
                int>(service, value)
            : Ping_service::rpc_async_ping(service, value);
        g_type_id_gate.enabled = false;
        write_stage(stage_path, "ping_sent");
        if (ping.get() != value + 1) {
            write_stage(stage_path, "ping_failed: wrong reply");
            probe.reset();
            return guard.leave() ? 7 : 8;
        }
    }
    catch (const std::exception& e) {
        g_type_id_gate.enabled = false;
        write_stage(stage_path, std::string("ping_failed: ") + e.what());
        probe.reset();
        return guard.leave() ? 7 : 8;
    }
    catch (...) {
        g_type_id_gate.enabled = false;
        write_stage(stage_path, "ping_failed: unknown exception");
        probe.reset();
        return guard.leave() ? 7 : 8;
    }
    write_stage(stage_path, "pinged");

    if (role == k_leaver_role &&
        !sintra::test::wait_for_file(entered_path, 10s, 1ms))
    {
        write_stage(stage_path, "entered_wait_failed");
        return guard.leave() ? 9 : 10;
    }

    probe.reset();
    const bool left = guard.leave();
    write_stage(stage_path, left ? "left" : "leave_failed");
    return left ? 0 : 11;
}

std::vector<std::string> child_arguments(
    const char*                                role,
    const std::filesystem::path&               stage_path,
    const std::filesystem::path&               entered_path,
    const std::filesystem::path&               release_path,
    int                                        value,
    const sintra::External_process_invitation& invitation)
{
    std::vector<std::string> arguments = {
        k_role_arg,
        role,
        k_stage_arg,
        stage_path.string(),
        k_entered_arg,
        entered_path.string(),
        k_release_arg,
        release_path.string(),
        k_probe_name_arg,
        k_probe_name,
        k_value_arg,
        std::to_string(value),
    };
    const auto sintra_arguments = invitation.sintra_args();
    arguments.insert(
        arguments.end(),
        sintra_arguments.begin(),
        sintra_arguments.end());
    return arguments;
}

bool launch_child(
    const std::string&              binary_path,
    const std::vector<std::string>& arguments,
    sintra::test::Exact_child&      child)
{
    std::vector<std::string> all_arguments{binary_path};
    all_arguments.insert(
        all_arguments.end(),
        arguments.begin(),
        arguments.end());
    sintra::C_string_vector c_arguments(all_arguments);
    return child.spawn(binary_path.c_str(), c_arguments.v());
}

bool wait_for_true(
    const std::atomic_bool&   value,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return value.load(std::memory_order_acquire);
}

bool wait_for_clean_exit(
    const char*                    role,
    const std::filesystem::path&   stage_path,
    sintra::test::Exact_child&     child,
    std::chrono::milliseconds      timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = child.poll();
        if (state == sintra::test::Exact_child_state::running) {
            std::this_thread::sleep_for(2ms);
            continue;
        }
        std::string settle_diagnostic;
        const bool clean =
            state == sintra::test::Exact_child_state::exited &&
            child.exited_with_code(0) &&
            child.settle_observed_exit(settle_diagnostic) &&
            read_stage(stage_path) == "left";
        if (!clean) {
            std::fprintf(
                stderr,
                "%s%s ended at stage '%s': %s%s%s\n",
                k_failure_prefix,
                role,
                read_stage(stage_path).c_str(),
                child.describe_status().c_str(),
                settle_diagnostic.empty() ? "" : "; ",
                settle_diagnostic.c_str());
        }
        return clean;
    }

    std::string cleanup_diagnostic;
    const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
    std::fprintf(
        stderr,
        "%s%s timed out at stage '%s' (pid %d); cleanup %s%s%s\n",
        k_failure_prefix,
        role,
        read_stage(stage_path).c_str(),
        child.pid(),
        cleaned ? "settled" : "failed",
        cleanup_diagnostic.empty() ? "" : ": ",
        cleanup_diagnostic.c_str());
    return false;
}

int run_parent(int argc, char* argv[])
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    Ping_service service;
    if (!service.assign_name(k_service_name)) {
        std::fprintf(stderr, "%scould not publish ping service\n", k_failure_prefix);
        return guard.shutdown() ? 1 : 2;
    }

    sintra::test::Shared_directory scratch(
        "SINTRA_FIRST_RPC_TEST_DIR",
        "external_first_rpc");
    const auto entered_path = scratch.path() / "type_entered.txt";
    const auto release_path = scratch.path() / "type_release.txt";
    const auto victim_stage = scratch.path() / "victim_stage.txt";
    const auto leaver_stage = scratch.path() / "leaver_stage.txt";

    sintra::External_process_invitation_options options;
    options.timeout = 15s;
    auto victim_invitation =
        sintra::create_external_process_invitation(options);
    auto leaver_invitation =
        sintra::create_external_process_invitation(options);
    if (!victim_invitation || !leaver_invitation) {
        std::fprintf(stderr, "%scould not create invitations\n", k_failure_prefix);
        return guard.shutdown() ? 1 : 2;
    }

    std::atomic_bool leaver_unpublished{false};
    const auto leaver_iid = leaver_invitation.process_instance_id;
    auto observe_leaver_unpublished =
        [&](const sintra::Coordinator::instance_unpublished& event) {
            if (event.instance_id == leaver_iid) {
                leaver_unpublished.store(true, std::memory_order_release);
            }
        };
    auto deactivate_unpublished = sintra::activate_slot(
        observe_leaver_unpublished,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    const std::string binary_path =
        sintra::test::get_binary_path(argc, argv);
    const auto victim_arguments = child_arguments(
        k_victim_role,
        victim_stage,
        entered_path,
        release_path,
        17,
        victim_invitation);
    const auto leaver_arguments = child_arguments(
        k_leaver_role,
        leaver_stage,
        entered_path,
        release_path,
        41,
        leaver_invitation);
    sintra::test::Exact_child victim(3s);
    sintra::test::Exact_child leaver(3s);
    std::atomic_bool launch{false};
    bool victim_launched = false;
    bool leaver_launched = false;
    std::thread victim_launcher([&]() {
        while (!launch.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        victim_launched =
            launch_child(binary_path, victim_arguments, victim);
    });
    std::thread leaver_launcher([&]() {
        while (!launch.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        leaver_launched =
            launch_child(binary_path, leaver_arguments, leaver);
    });
    launch.store(true, std::memory_order_release);
    victim_launcher.join();
    leaver_launcher.join();

    bool ok = victim_launched && leaver_launched;
    if (!ok) {
        std::fprintf(
            stderr,
            "%schild launch failed (victim: %s; leaver: %s)\n",
            k_failure_prefix,
            victim.error().c_str(),
            leaver.error().c_str());
    }

    const bool entered = ok &&
        sintra::test::wait_for_file(entered_path, 10s, 1ms);
    if (ok && !entered) {
        std::fprintf(
            stderr,
            "%svictim did not enter its first type-id gate; stage '%s'\n",
            k_failure_prefix,
            read_stage(victim_stage).c_str());
    }

    const bool unpublished = entered &&
        wait_for_true(leaver_unpublished, 5s);
    if (entered && !unpublished) {
        std::fprintf(
            stderr,
            "%sleaver did not unpublish after victim entered the type-id gate\n",
            k_failure_prefix);
    }

    bool probe_completed = false;
    if (unpublished) {
        try {
            const auto probe_iid =
                sintra::Coordinator::rpc_resolve_instance(
                    sintra::s_coord_id,
                    k_probe_name);
            if (probe_iid == sintra::invalid_instance_id) {
                throw std::runtime_error("victim probe service did not resolve");
            }
            auto probe = Probe_service::rpc_async_probe(probe_iid, 23);
            probe_completed =
                probe.get_until(std::chrono::steady_clock::now() + 5s) == 24;
        }
        catch (const std::exception& e) {
            std::fprintf(
                stderr,
                "%sordered victim probe failed: %s\n",
                k_failure_prefix,
                e.what());
        }
    }
    if (unpublished && !probe_completed) {
        std::fprintf(
            stderr,
            "%svictim did not process the leaver unpublish before type release\n",
            k_failure_prefix);
    }
    sintra::test::write_lines(release_path, {"release"});

    ok = ok && entered && unpublished && probe_completed;
    if (victim_launched) {
        ok = wait_for_clean_exit(
            k_victim_role,
            victim_stage,
            victim,
            10s) && ok;
    }
    if (leaver_launched) {
        ok = wait_for_clean_exit(
            k_leaver_role,
            leaver_stage,
            leaver,
            10s) && ok;
    }
    deactivate_unpublished();
    return guard.shutdown() && ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    const std::string role =
        sintra::test::get_argv_value(argc, argv, k_role_arg);
    if (role == k_victim_role || role == k_leaver_role) {
        return run_child(argc, argv);
    }
    return run_parent(argc, argv);
}
