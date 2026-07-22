#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* k_role_arg       = "--rpc-caller-role";
constexpr const char* k_service_name   = "rpc_caller_identity_service";
constexpr const char* k_failure_prefix = "rpc_caller_identity_test: ";
constexpr std::size_t k_child_count    = 2;

struct Identity_probe : sintra::Derived_transceiver<Identity_probe>
{
    sintra::instance_id_type direct_identity()
    {
        return sintra::current_rpc_caller();
    }

    sintra::instance_id_type transported_identity()
    {
        return sintra::current_rpc_caller();
    }

    void throw_from_direct_identity()
    {
        if (sintra::current_rpc_caller() != sintra::invalid_instance_id) {
            throw std::domain_error("Direct RPC inherited a transported caller");
        }
        throw std::runtime_error("Expected direct RPC exception");
    }

    SINTRA_RPC(direct_identity)
    SINTRA_RPC_STRICT(transported_identity)
    SINTRA_RPC(throw_from_direct_identity)
};

class Caller_service : public sintra::Derived_transceiver<Caller_service>
{
public:
    Caller_service(
        sintra::instance_id_type authorized_process,
        sintra::instance_id_type nested_probe)
    :
        m_authorized_process(authorized_process),
        m_nested_probe(nested_probe)
    {}

    sintra::instance_id_type concurrent_identity(std::uint32_t child_index)
    {
        const sintra::instance_id_type before = sintra::current_rpc_caller();
        if (child_index >= k_child_count) {
            throw std::out_of_range("RPC caller test child index is invalid");
        }

        {
            std::unique_lock<std::mutex> lock(m_concurrent_mutex);
            m_concurrent_callers[child_index] = before;
            ++m_concurrent_entered;
            m_concurrent_condition.notify_all();
            if (!m_concurrent_condition.wait_for(
                    lock,
                    std::chrono::seconds(10),
                    [&]() { return m_concurrent_entered == k_child_count; }))
            {
                throw std::runtime_error("Timed out waiting for concurrent RPC callers");
            }
        }

        if (sintra::current_rpc_caller() != before) {
            throw std::runtime_error("Concurrent RPC caller identity changed in handler");
        }
        return before;
    }

    sintra::instance_id_type authorized_identity()
    {
        const sintra::instance_id_type caller = sintra::current_rpc_caller();
        if (caller != m_authorized_process) {
            m_authorization_denials.fetch_add(1, std::memory_order_relaxed);
            throw std::domain_error("RPC caller is not authorized");
        }
        m_authorization_accepts.fetch_add(1, std::memory_order_relaxed);
        return caller;
    }

    sintra::instance_id_type nested_identity()
    {
        const sintra::instance_id_type outer_before =
            sintra::current_rpc_caller();
        const sintra::instance_id_type direct_inner =
            Identity_probe::rpc_direct_identity(m_nested_probe);
        const sintra::instance_id_type outer_after_direct =
            sintra::current_rpc_caller();

        bool received_expected_direct_exception = false;
        try {
            Identity_probe::rpc_throw_from_direct_identity(m_nested_probe);
        }
        catch (const std::runtime_error& e) {
            received_expected_direct_exception =
                std::string(e.what()) == "Expected direct RPC exception";
        }
        const sintra::instance_id_type outer_after_exception =
            sintra::current_rpc_caller();

        const sintra::instance_id_type transported_inner =
            Identity_probe::rpc_transported_identity(m_nested_probe);
        const sintra::instance_id_type outer_after_transport =
            sintra::current_rpc_caller();

        if (direct_inner != sintra::invalid_instance_id ||
            outer_after_direct != outer_before ||
            !received_expected_direct_exception ||
            outer_after_exception != outer_before ||
            transported_inner != sintra::s_mproc_id ||
            outer_after_transport != outer_before)
        {
            throw std::runtime_error("Nested RPC caller identities were not isolated");
        }
        return outer_after_transport;
    }

    sintra::instance_id_type returning_identity()
    {
        schedule_post_handler_check(&m_return_post_checks);
        return sintra::current_rpc_caller();
    }

    void throwing_identity()
    {
        schedule_post_handler_check(&m_exception_post_checks);
        throw std::runtime_error("Expected RPC handler exception");
    }

    sintra::instance_id_type concurrent_caller(std::size_t index) const
    {
        return m_concurrent_callers[index].load(std::memory_order_acquire);
    }

    unsigned authorization_accepts() const
    {
        return m_authorization_accepts.load(std::memory_order_acquire);
    }

    unsigned authorization_denials() const
    {
        return m_authorization_denials.load(std::memory_order_acquire);
    }

    unsigned return_post_checks() const
    {
        return m_return_post_checks.load(std::memory_order_acquire);
    }

    unsigned exception_post_checks() const
    {
        return m_exception_post_checks.load(std::memory_order_acquire);
    }

    unsigned post_check_failures() const
    {
        return m_post_check_failures.load(std::memory_order_acquire);
    }

    SINTRA_RPC(concurrent_identity)
    SINTRA_RPC(authorized_identity)
    SINTRA_RPC(nested_identity)
    SINTRA_RPC(returning_identity)
    SINTRA_RPC(throwing_identity)

private:
    void schedule_post_handler_check(std::atomic_uint* completed_checks)
    {
        if (sintra::tl_post_handler_function_ready()) {
            throw std::runtime_error("Unexpected existing RPC post-handler task");
        }
        sintra::tl_post_handler_function_ref() = [this, completed_checks]() {
            if (sintra::current_rpc_caller() != sintra::invalid_instance_id) {
                m_post_check_failures.fetch_add(1, std::memory_order_relaxed);
            }
            completed_checks->fetch_add(1, std::memory_order_release);
        };
    }

    const sintra::instance_id_type m_authorized_process;
    const sintra::instance_id_type m_nested_probe;
    std::mutex                     m_concurrent_mutex;
    std::condition_variable        m_concurrent_condition;
    std::size_t                    m_concurrent_entered = 0;

    std::array<std::atomic<sintra::instance_id_type>, k_child_count> m_concurrent_callers{};

    std::atomic_uint                m_authorization_accepts{0};
    std::atomic_uint                m_authorization_denials{0};
    std::atomic_uint                m_return_post_checks{0};
    std::atomic_uint                m_exception_post_checks{0};
    std::atomic_uint                m_post_check_failures{0};
};

struct Runtime_guard
{
    bool m_active = false;

    ~Runtime_guard()
    {
        if (m_active && sintra::s_mproc) {
            try {
                sintra::detail::finalize();
            }
            catch (...) {
            }
        }
    }

    bool shutdown()
    {
        if (!m_active) {
            return true;
        }
        m_active = false;
        return sintra::shutdown();
    }
};

int run_child(int argc, char* argv[], std::uint32_t child_index)
{
    try {
        sintra::init(argc, argv);
        const sintra::instance_id_type self = sintra::s_mproc_id;
        const sintra::instance_id_type service =
            sintra::Coordinator::rpc_resolve_instance(
                sintra::s_coord_id,
                k_service_name);
        if (service == sintra::invalid_instance_id ||
            sintra::current_rpc_caller() != sintra::invalid_instance_id)
        {
            sintra::leave();
            return 2;
        }

        if (Caller_service::rpc_concurrent_identity(service, child_index) != self) {
            sintra::leave();
            return 3;
        }

        if (child_index == 0) {
            if (Caller_service::rpc_authorized_identity(service) != self ||
                Caller_service::rpc_nested_identity(service) != self ||
                Caller_service::rpc_returning_identity(service) != self)
            {
                sintra::leave();
                return 4;
            }

            bool received_expected_exception = false;
            try {
                Caller_service::rpc_throwing_identity(service);
            }
            catch (const std::runtime_error& e) {
                received_expected_exception =
                    std::string(e.what()) == "Expected RPC handler exception";
            }
            if (!received_expected_exception) {
                sintra::leave();
                return 5;
            }
        }
        else {
            bool denied = false;
            try {
                (void)Caller_service::rpc_authorized_identity(service);
            }
            catch (const std::domain_error&) {
                denied = true;
            }
            if (!denied) {
                sintra::leave();
                return 6;
            }
        }

        const bool outside_handler_is_invalid =
            sintra::current_rpc_caller() == sintra::invalid_instance_id;
        sintra::leave();
        return outside_handler_is_invalid ? 0 : 7;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%schild %u failed: %s\n",
            k_failure_prefix, child_index, e.what());
    }
    catch (...) {
        std::fprintf(stderr, "%schild %u failed with an unknown exception\n",
            k_failure_prefix, child_index);
    }
    if (sintra::s_mproc) {
        sintra::leave();
    }
    return 8;
}

std::vector<std::string> child_arguments(
    std::uint32_t                              child_index,
    const sintra::External_process_invitation& invitation)
{
    std::vector<std::string> arguments = {
        k_role_arg,
        std::to_string(child_index),
    };
    std::vector<std::string> sintra_arguments = invitation.sintra_args();
    arguments.insert(
        arguments.end(),
        sintra_arguments.begin(),
        sintra_arguments.end());
    return arguments;
}

bool wait_for_children(
    std::array<sintra::test::Exact_child, k_child_count>& children)
{
    std::array<bool, k_child_count> settled{};
    std::size_t remaining = k_child_count;
    bool ok = true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);

    while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < k_child_count; ++i) {
            if (settled[i]) {
                continue;
            }
            const auto state = children[i].poll();
            if (state == sintra::test::Exact_child_state::running) {
                continue;
            }

            std::string diagnostic;
            const bool child_ok =
                state == sintra::test::Exact_child_state::exited &&
                children[i].exited_with_code(0) &&
                children[i].settle_observed_exit(diagnostic);
            if (!child_ok) {
                std::fprintf(stderr, "%schild %zu failed: %s%s%s\n",
                    k_failure_prefix,
                    i,
                    children[i].describe_status().c_str(),
                    diagnostic.empty() ? "" : "; ",
                    diagnostic.c_str());
                ok = false;
            }
            settled[i] = true;
            --remaining;
        }
        if (remaining > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    for (std::size_t i = 0; i < k_child_count; ++i) {
        if (settled[i]) {
            continue;
        }
        std::string diagnostic;
        const bool cleaned = children[i].terminate_and_settle(diagnostic);
        std::fprintf(stderr, "%schild %zu timed out; cleanup %s%s%s\n",
            k_failure_prefix,
            i,
            cleaned ? "settled" : "failed",
            diagnostic.empty() ? "" : ": ",
            diagnostic.c_str());
        ok = false;
    }
    return ok;
}

int run_parent(int argc, char* argv[])
{
    bool ok = sintra::current_rpc_caller() == sintra::invalid_instance_id;
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    Identity_probe probe;
    ok &= Identity_probe::rpc_direct_identity(probe.instance_id()) ==
        sintra::invalid_instance_id;
    ok &= Identity_probe::rpc_transported_identity(probe.instance_id()) ==
        sintra::s_mproc_id;

    std::array<sintra::External_process_invitation, k_child_count> invitations;
    sintra::External_process_invitation_options options;
    options.timeout = std::chrono::seconds(20);
    for (auto& invitation : invitations) {
        invitation = sintra::create_external_process_invitation(options);
        ok &= static_cast<bool>(invitation);
    }
    if (!ok) {
        std::fprintf(stderr, "%ssetup failed\n", k_failure_prefix);
        return guard.shutdown() ? 1 : 2;
    }

    Caller_service service(
        invitations[0].process_instance_id,
        probe.instance_id());
    if (!service.assign_name(k_service_name)) {
        std::fprintf(stderr, "%scould not publish service\n", k_failure_prefix);
        return guard.shutdown() ? 1 : 2;
    }

    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    std::array<sintra::test::Exact_child, k_child_count> children = {
        sintra::test::Exact_child(std::chrono::seconds(3)),
        sintra::test::Exact_child(std::chrono::seconds(3)),
    };
    for (std::size_t i = 0; i < k_child_count; ++i) {
        std::vector<std::string> arguments = child_arguments(
            static_cast<std::uint32_t>(i),
            invitations[i]);
        arguments.insert(arguments.begin(), binary_path);
        sintra::C_string_vector c_arguments(arguments);
        if (!children[i].spawn(binary_path.c_str(), c_arguments.v())) {
            std::fprintf(stderr, "%scould not spawn child %zu: %s\n",
                k_failure_prefix, i, children[i].error().c_str());
            ok = false;
            break;
        }
    }
    ok &= wait_for_children(children);

    const auto post_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((service.return_post_checks() != 1 ||
            service.exception_post_checks() != 1) &&
           std::chrono::steady_clock::now() < post_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (std::size_t i = 0; i < k_child_count; ++i) {
        ok &= service.concurrent_caller(i) ==
            invitations[i].process_instance_id;
    }
    ok &= service.authorization_accepts() == 1;
    ok &= service.authorization_denials() == 1;
    ok &= service.return_post_checks() == 1;
    ok &= service.exception_post_checks() == 1;
    ok &= service.post_check_failures() == 0;
    ok &= sintra::current_rpc_caller() == sintra::invalid_instance_id;

    if (!ok) {
        std::fprintf(stderr, "%sidentity contract failed\n", k_failure_prefix);
    }
    return guard.shutdown() && ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    const std::string role =
        sintra::test::get_argv_value(argc, argv, k_role_arg);
    if (!role.empty()) {
        return run_child(
            argc,
            argv,
            static_cast<std::uint32_t>(std::stoul(role)));
    }
    return run_parent(argc, argv);
}
