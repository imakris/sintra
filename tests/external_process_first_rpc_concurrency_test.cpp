#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <array>
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

constexpr const char* k_role_arg       = "--first-rpc-role";
constexpr const char* k_stage_arg      = "--first-rpc-stage";
constexpr const char* k_value_arg      = "--first-rpc-value";
constexpr const char* k_entered_arg    = "--first-rpc-entered";
constexpr const char* k_release_arg    = "--first-rpc-release";
constexpr const char* k_probe_name_arg = "--first-rpc-probe-name";
constexpr const char* k_victim_role    = "victim";
constexpr const char* k_leaver_role    = "leaver";
constexpr const char* k_peer_role      = "peer";
constexpr const char* k_service_name   = "external_first_rpc_service";
constexpr const char* k_failure_prefix =
    "external_process_first_rpc_concurrency_test: ";

constexpr std::size_t k_child_count = 3;
constexpr int         k_round_count = 4;
constexpr std::size_t k_leaver_index = 1;

std::array<std::atomic_bool, k_child_count * k_round_count> g_ping_observed{};

std::size_t observation_index(int value)
{
    return
        static_cast<std::size_t>(value / 100) * k_child_count +
        static_cast<std::size_t>(value % 100);
}

struct Ping_service : sintra::Derived_transceiver<Ping_service>
{
    int ping(int value)
    {
        const std::size_t index = observation_index(value);
        if (index < g_ping_observed.size()) {
            g_ping_observed[index].store(true, std::memory_order_release);
        }
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

bool wait_for_file(
    const std::filesystem::path& path,
    std::chrono::milliseconds   timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::error_code error;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path, error)) {
            return true;
        }
        error.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::filesystem::exists(path, error);
}

struct Blocking_ping_message : Ping_message
{
    using Ping_message::Ping_message;

    static sintra::type_id_type id()
    {
        if (g_type_id_gate.enabled) {
            sintra::test::write_lines(g_type_id_gate.entered, {"entered"});
            if (!wait_for_file(
                    g_type_id_gate.release,
                    std::chrono::seconds(10)))
            {
                throw std::runtime_error("Timed out waiting to release type resolution");
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
};

void write_stage(
    const std::filesystem::path&   path,
    const std::string&             stage)
{
    sintra::test::write_lines(path, {stage});
}

std::string read_stage(const std::filesystem::path& path)
{
    const std::vector<std::string> lines = sintra::test::read_lines(path);
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
    write_stage(stage_path, "initialized");

    const sintra::instance_id_type service =
        sintra::Coordinator::rpc_resolve_instance(
            sintra::s_coord_id,
            k_service_name);
    if (service == sintra::invalid_instance_id) {
        write_stage(stage_path, "resolve_failed");
        sintra::leave();
        return 3;
    }
    write_stage(stage_path, "resolved");

    std::unique_ptr<Probe_service> probe;
    if (role == k_victim_role) {
        probe = std::make_unique<Probe_service>();
        if (!probe->assign_name(probe_name)) {
            write_stage(stage_path, "probe_publish_failed");
            probe.reset();
            sintra::leave();
            return 4;
        }
        g_type_id_gate = {true, entered_path, release_path};
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
            sintra::leave();
            return 5;
        }
    }
    catch (const std::exception& e) {
        g_type_id_gate.enabled = false;
        write_stage(stage_path, std::string("ping_failed: ") + e.what());
        probe.reset();
        sintra::leave();
        return 5;
    }
    catch (...) {
        g_type_id_gate.enabled = false;
        write_stage(stage_path, "ping_failed: unknown exception");
        probe.reset();
        sintra::leave();
        return 5;
    }
    write_stage(stage_path, "pinged");

    const std::filesystem::path wait_path = role == k_leaver_role
        ? entered_path
        : release_path;
    if (role != k_victim_role &&
        !wait_for_file(wait_path, std::chrono::seconds(10)))
    {
        write_stage(stage_path, "gate_wait_failed");
        sintra::leave();
        return 6;
    }

    probe.reset();
    sintra::leave();
    write_stage(stage_path, "left");
    return 0;
}

std::vector<std::string> child_arguments(
    const char*                                role,
    const std::filesystem::path&               stage_path,
    const std::filesystem::path&               entered_path,
    const std::filesystem::path&               release_path,
    const std::string&                         probe_name,
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
        probe_name,
        k_value_arg,
        std::to_string(value),
    };
    std::vector<std::string> sintra_arguments = invitation.sintra_args();
    arguments.insert(
        arguments.end(),
        sintra_arguments.begin(),
        sintra_arguments.end());
    return arguments;
}

bool run_round(
    int                            round,
    const std::string&             binary_path,
    const std::filesystem::path&   scratch)
{
    std::array<sintra::External_process_invitation, k_child_count> invitations;
    sintra::External_process_invitation_options invitation_options;
    invitation_options.timeout = std::chrono::seconds(15);
    for (auto& invitation : invitations) {
        invitation = sintra::create_external_process_invitation(
            invitation_options);
        if (!invitation) {
            std::fprintf(
                stderr,
                "%sround %d could not create all invitations\n",
                k_failure_prefix,
                round);
            return false;
        }
    }

    const sintra::instance_id_type leaver_iid =
        invitations[k_leaver_index].process_instance_id;
    std::atomic_bool leaver_unpublished{false};
    auto observe_leaver_unpublished =
        [&](const sintra::Coordinator::instance_unpublished& event) {
            if (event.instance_id == leaver_iid) {
                leaver_unpublished.store(true, std::memory_order_release);
            }
        };
    auto deactivate_unpublished = sintra::activate_slot(
        observe_leaver_unpublished,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    const std::array<const char*, k_child_count> roles = {
        k_victim_role,
        k_leaver_role,
        k_peer_role,
    };
    const std::filesystem::path entered_path = scratch /
        ("round_" + std::to_string(round) + "_type_entered.txt");
    const std::filesystem::path release_path = scratch /
        ("round_" + std::to_string(round) + "_type_release.txt");
    const std::string probe_name =
        "external_first_rpc_probe_" + std::to_string(round);
    std::array<std::filesystem::path, k_child_count> stage_paths;
    std::array<std::vector<std::string>, k_child_count> arguments;
    std::array<std::unique_ptr<sintra::test::Exact_child>, k_child_count>
        children;
    for (std::size_t i = 0; i < k_child_count; ++i) {
        stage_paths[i] = scratch /
            ("round_" + std::to_string(round) + "_child_" +
             std::to_string(i) + ".txt");
        arguments[i] = child_arguments(
            roles[i],
            stage_paths[i],
            entered_path,
            release_path,
            probe_name,
            round * 100 + static_cast<int>(i),
            invitations[i]);
        children[i] = std::make_unique<sintra::test::Exact_child>(
            std::chrono::seconds(3));
    }

    std::atomic_bool launch{false};
    std::array<bool, k_child_count> launched{};
    std::array<std::thread, k_child_count> launch_threads;
    for (std::size_t i = 0; i < k_child_count; ++i) {
        launch_threads[i] = std::thread([&, i]() {
            while (!launch.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::vector<std::string> all_arguments;
            all_arguments.reserve(arguments[i].size() + 1);
            all_arguments.push_back(binary_path);
            all_arguments.insert(
                all_arguments.end(),
                arguments[i].begin(),
                arguments[i].end());
            sintra::C_string_vector c_arguments(all_arguments);
            launched[i] = children[i]->spawn(
                binary_path.c_str(),
                c_arguments.v());
        });
    }
    launch.store(true, std::memory_order_release);
    for (std::thread& thread : launch_threads) {
        thread.join();
    }

    bool ok = true;
    for (std::size_t i = 0; i < k_child_count; ++i) {
        if (!launched[i]) {
            std::fprintf(
                stderr,
                "%sround %d child %zu launch failed: %s\n",
                k_failure_prefix,
                round,
                i,
                children[i]->error().c_str());
            ok = false;
        }
    }
    if (!ok) {
        deactivate_unpublished();
        return false;
    }

    std::array<bool, k_child_count> settled{};
    std::size_t remaining = k_child_count;
    bool release_written = false;
    bool probe_completed = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < k_child_count; ++i) {
            if (settled[i]) {
                continue;
            }
            const sintra::test::Exact_child_state state = children[i]->poll();
            if (state == sintra::test::Exact_child_state::running) {
                continue;
            }
            const std::string status = children[i]->describe_status();
            std::string settle_diagnostic;
            const bool clean =
                state == sintra::test::Exact_child_state::exited &&
                children[i]->exited_with_code(0) &&
                children[i]->settle_observed_exit(settle_diagnostic);
            if (!clean || read_stage(stage_paths[i]) != "left") {
                std::fprintf(
                    stderr,
                    "%sround %d child %zu ended at stage '%s': %s%s%s\n",
                    k_failure_prefix,
                    round,
                    i,
                    read_stage(stage_paths[i]).c_str(),
                    status.c_str(),
                    settle_diagnostic.empty() ? "" : "; ",
                    settle_diagnostic.c_str());
                ok = false;
            }
            settled[i] = true;
            --remaining;
        }

        if (!release_written &&
            leaver_unpublished.load(std::memory_order_acquire))
        {
            try {
                const sintra::instance_id_type probe_iid =
                    sintra::Coordinator::rpc_resolve_instance(
                        sintra::s_coord_id,
                        probe_name);
                if (probe_iid == sintra::invalid_instance_id) {
                    throw std::runtime_error("victim probe service did not resolve");
                }
                auto probe = Probe_service::rpc_async_probe(
                    probe_iid,
                    round);
                probe_completed = probe.get_until(deadline) == round + 1;
            }
            catch (const std::exception& e) {
                std::fprintf(
                    stderr,
                    "%sround %d ordered probe failed: %s\n",
                    k_failure_prefix,
                    round,
                    e.what());
            }
            release_written = true;
            sintra::test::write_lines(release_path, {"release"});
            if (!probe_completed) {
                ok = false;
            }
        }
        if (remaining > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (!release_written) {
        sintra::test::write_lines(release_path, {"release"});
        std::fprintf(
            stderr,
            "%sround %d leaver did not reach the ordered probe fence\n",
            k_failure_prefix,
            round);
        ok = false;
    }

    for (std::size_t i = 0; i < k_child_count; ++i) {
        if (settled[i]) {
            continue;
        }
        std::string cleanup_diagnostic;
        const bool cleaned =
            children[i]->terminate_and_settle(cleanup_diagnostic);
        std::fprintf(
            stderr,
            "%sround %d child %zu timed out at stage '%s' (pid %d, handler %s); "
            "cleanup %s%s%s\n",
            k_failure_prefix,
            round,
            i,
            read_stage(stage_paths[i]).c_str(),
            children[i]->pid(),
            g_ping_observed[observation_index(
                round * 100 + static_cast<int>(i))].load(
                    std::memory_order_acquire)
                ? "observed"
                : "not observed",
            cleaned ? "settled" : "failed",
            cleanup_diagnostic.empty() ? "" : ": ",
            cleanup_diagnostic.c_str());
        ok = false;
    }
    deactivate_unpublished();
    return ok;
}

int run_parent(int argc, char* argv[])
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    Ping_service service;
    if (!service.assign_name(k_service_name)) {
        std::fprintf(
            stderr,
            "%scould not publish ping service\n",
            k_failure_prefix);
        return guard.shutdown() ? 1 : 2;
    }

    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    sintra::test::Shared_directory scratch(
        "SINTRA_FIRST_RPC_TEST_DIR",
        "external_first_rpc");
    bool ok = true;
    for (int round = 0; round < k_round_count && ok; ++round) {
        ok = run_round(round, binary_path, scratch.path());
    }
    return guard.shutdown() && ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    const std::string role =
        sintra::test::get_argv_value(argc, argv, k_role_arg);
    if (role == k_victim_role || role == k_leaver_role || role == k_peer_role) {
        return run_child(argc, argv);
    }
    return run_parent(argc, argv);
}
