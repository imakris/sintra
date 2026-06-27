// Tests for shutdown/lifecycle fail-fast guardrails.
//
// Verifies:
// 1. detail::finalize() rejects for every non-idle shutdown protocol state.
// 2. shutdown() rejects for every non-idle shutdown protocol state.
// 3. leave() rejects for every non-idle shutdown protocol state.
// 4. barrier() rejects on _sintra_all_processes while teardown is active.
// 5. shutdown() without an active runtime returns false and resets state.
// 6. leave() without an active runtime returns false and resets state.
// 7. finalize() without an active runtime returns false.
// 8. Protocol state returns to idle after successful finalize/leave.
// 9. leave() rejects when called from handler dispatch context.
// 10. Double init() without teardown is rejected.
// 11. Runtime-owned public APIs reject missing runtime cleanly.
// 12. init()/finalize() cycles work correctly (state resets between cycles).
// 13. Process-entry APIs reject while teardown admission is closed.
// 14. Message-reader diagnostic helpers report stable names and missing-reader state.
// 15. Runtime process-entry APIs take cheap no-runtime/no-spawn exits.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <array>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view k_prefix = "lifecycle_guardrails_test: ";

struct state_case_t
{
    sintra::detail::shutdown_protocol_state state;
    const char* name;
};

constexpr std::array<state_case_t, 5> k_non_idle_states{{
    {sintra::detail::shutdown_protocol_state::collective_shutdown_entered, "collective_shutdown_entered"},
    {sintra::detail::shutdown_protocol_state::local_departure_entered, "local_departure_entered"},
    {sintra::detail::shutdown_protocol_state::coordinator_hook_running, "coordinator_hook_running"},
    {sintra::detail::shutdown_protocol_state::coordinator_hook_completed, "coordinator_hook_completed"},
    {sintra::detail::shutdown_protocol_state::finalizing, "finalizing"},
}};

void reset_shutdown_state()
{
    sintra::detail::reset_lifecycle_teardown_to_idle();
}

struct Teardown_admission_guard
{
    Teardown_admission_guard()
    {
        sintra::detail::close_teardown_admission_and_claim_state(
            sintra::detail::shutdown_protocol_state::finalizing,
            "lifecycle_guardrails_test");
    }

    ~Teardown_admission_guard()
    {
        sintra::detail::reset_lifecycle_teardown_to_idle();
    }
};

bool expect_finalize_rejected_for_state(state_case_t state_case)
{
    reset_shutdown_state();
    sintra::detail::s_shutdown_state.store(state_case.state, std::memory_order_release);

    try {
        (void)sintra::detail::finalize();
    }
    catch (const std::logic_error&) {
        reset_shutdown_state();
        return true;
    }

    reset_shutdown_state();
    return false;
}

bool expect_shutdown_rejected_for_state(state_case_t state_case)
{
    reset_shutdown_state();
    sintra::detail::s_shutdown_state.store(state_case.state, std::memory_order_release);

    try {
        (void)sintra::shutdown();
    }
    catch (const std::logic_error&) {
        reset_shutdown_state();
        return true;
    }

    reset_shutdown_state();
    return false;
}

bool expect_leave_rejected_for_state(state_case_t state_case)
{
    reset_shutdown_state();
    sintra::detail::s_shutdown_state.store(state_case.state, std::memory_order_release);

    try {
        (void)sintra::leave();
    }
    catch (const std::logic_error&) {
        reset_shutdown_state();
        return true;
    }

    reset_shutdown_state();
    return false;
}

bool test_finalize_rejects_for_all_active_states()
{
    bool ok = true;
    for (const auto& state_case : k_non_idle_states) {
        ok &= sintra::test::assert_true(
            expect_finalize_rejected_for_state(state_case),
            k_prefix,
            std::string("detail::finalize() should throw for state ") + state_case.name);
    }
    return ok;
}

bool test_shutdown_rejects_for_all_active_states()
{
    bool ok = true;
    for (const auto& state_case : k_non_idle_states) {
        ok &= sintra::test::assert_true(
            expect_shutdown_rejected_for_state(state_case),
            k_prefix,
            std::string("shutdown() should throw for state ") + state_case.name);
    }
    return ok;
}

bool test_leave_rejects_for_all_active_states()
{
    bool ok = true;
    for (const auto& state_case : k_non_idle_states) {
        ok &= sintra::test::assert_true(
            expect_leave_rejected_for_state(state_case),
            k_prefix,
            std::string("leave() should throw for state ") + state_case.name);
    }
    return ok;
}

bool expect_barrier_rejected_for_state(state_case_t state_case)
{
    reset_shutdown_state();
    sintra::detail::s_shutdown_state.store(state_case.state, std::memory_order_release);

    try {
        (void)sintra::barrier("guardrail-probe", "_sintra_all_processes");
    }
    catch (const std::logic_error&) {
        reset_shutdown_state();
        return true;
    }

    reset_shutdown_state();
    return false;
}

bool test_barrier_rejects_during_active_teardown()
{
    constexpr std::array<state_case_t, 2> k_barrier_guardrail_states{{
        {sintra::detail::shutdown_protocol_state::collective_shutdown_entered, "collective_shutdown_entered"},
        {sintra::detail::shutdown_protocol_state::local_departure_entered, "local_departure_entered"},
    }};

    bool ok = true;
    for (const auto& state_case : k_barrier_guardrail_states) {
        ok &= sintra::test::assert_true(
            expect_barrier_rejected_for_state(state_case),
            k_prefix,
            std::string("barrier() should throw on _sintra_all_processes while teardown is active in state ") +
                state_case.name);
    }
    return ok;
}

bool test_leave_without_runtime_returns_false()
{
    reset_shutdown_state();

    const bool result      = sintra::leave();
    const auto state_after =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);

    return
        sintra::test::assert_true(
            !result && state_after == sintra::detail::shutdown_protocol_state::idle,
            k_prefix,
            "leave() without init() should return false and leave state idle"
        );
}

bool test_shutdown_without_runtime_returns_false()
{
    reset_shutdown_state();

    const bool result      = sintra::shutdown();
    const auto state_after =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);

    return
        sintra::test::assert_true(
            !result && state_after == sintra::detail::shutdown_protocol_state::idle,
            k_prefix,
            "shutdown() without init() should return false and leave state idle"
        );
}

bool test_state_idle_after_finalize(int argc, char* argv[])
{
    sintra::init(argc, argv);

    const auto before = sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
    if (before != sintra::detail::shutdown_protocol_state::idle) {
        sintra::test::print_test_message(
            k_prefix,
            "state should be idle before finalize");
        sintra::detail::finalize_impl();
        return false;
    }

    sintra::detail::finalize();

    const auto after = sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
    return
        sintra::test::assert_true(
            after == sintra::detail::shutdown_protocol_state::idle,
            k_prefix,
            "state should be idle after finalize"
        );
}

bool test_finalize_without_init_returns_false()
{
    reset_shutdown_state();

    const bool result = sintra::detail::finalize();

    return sintra::test::assert_true(
        !result,
        k_prefix,
        "finalize() without init() should return false");
}

bool test_runtime_required_apis_without_runtime_throw()
{
    reset_shutdown_state();

    bool activate_caught = false;
    try {
        (void)sintra::activate_slot([](int) {});
    }
    catch (const std::runtime_error&) {
        activate_caught = true;
    }

    bool deactivate_caught = false;
    try {
        sintra::deactivate_all_slots();
    }
    catch (const std::runtime_error&) {
        deactivate_caught = true;
    }

    bool recovery_caught = false;
    try {
        sintra::enable_recovery();
    }
    catch (const std::runtime_error&) {
        recovery_caught = true;
    }

    return
        sintra::test::assert_true(
            activate_caught && deactivate_caught && recovery_caught,
            k_prefix,
            "runtime-owned APIs without init() should throw runtime_error instead of " "dereferencing runtime state"
        );
}

bool test_runtime_process_entry_early_returns(int argc, char* argv[])
{
    reset_shutdown_state();

    bool ok = true;

    ok &= sintra::test::assert_true(
        !sintra::create_external_process_invitation(),
        k_prefix,
        "create_external_process_invitation() without runtime should return an invalid invitation");

    sintra::Spawn_options empty_spawn_options;
    ok &= sintra::test::assert_true(
        sintra::spawn_swarm_process(empty_spawn_options) == 0,
        k_prefix,
        "spawn_swarm_process() with empty binary should return 0 without runtime");

    ok &= sintra::test::assert_true(
        sintra::join_swarm(0) == sintra::invalid_instance_id,
        k_prefix,
        "join_swarm() with an invalid branch index should return invalid_instance_id");

    sintra::init(argc, argv);

    sintra::External_process_invitation_options timeout_options;
    timeout_options.timeout = std::chrono::milliseconds{0};
    ok &= sintra::test::assert_true(
        !sintra::create_external_process_invitation(timeout_options),
        k_prefix,
        "create_external_process_invitation() with zero timeout should return an invalid invitation");

    sintra::External_process_invitation_options invalid_id_options;
    invalid_id_options.process_instance_id = sintra::compose_instance(2, 2);
    ok &= sintra::test::assert_true(
        !sintra::create_external_process_invitation(invalid_id_options),
        k_prefix,
        "create_external_process_invitation() with a non-process instance id should return an invalid invitation");

    ok &= sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(sintra::invalid_instance_id),
        k_prefix,
        "cancel_external_process_invitation() with invalid_instance_id should return false");

    ok &= sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(sintra::External_process_invitation{}),
        k_prefix,
        "cancel_external_process_invitation() with a default invitation should return false");

    ok &= sintra::test::assert_true(
        sintra::detail::finalize(),
        k_prefix,
        "finalize() should succeed after runtime process-entry early-return checks");

    return ok;
}

bool test_process_message_reader_diagnostic_helpers()
{
    reset_shutdown_state();

    using Reader = sintra::Process_message_reader;

    bool ok = true;
    ok &= sintra::test::assert_true(
        std::string(Reader::reader_state_name(Reader::READER_NORMAL)) == "normal" &&
        std::string(Reader::reader_state_name(Reader::READER_SERVICE)) == "service" &&
        std::string(Reader::reader_state_name(Reader::READER_STOPPING)) == "stopping" &&
        std::string(Reader::reader_state_name(static_cast<Reader::State>(99))) == "unknown",
        k_prefix,
        "reader_state_name() should cover known and unknown states");

    ok &= sintra::test::assert_true(
        std::string(Reader::delivery_stream_name(Reader::Delivery_stream::Request)) == "request" &&
        std::string(Reader::delivery_stream_name(Reader::Delivery_stream::Reply)) == "reply" &&
        std::string(Reader::delivery_stream_name(static_cast<Reader::Delivery_stream>(99))) == "unknown",
        k_prefix,
        "delivery_stream_name() should cover known and unknown streams");

    ok &= sintra::test::assert_true(
        std::string(Reader::communication_state_name(sintra::Managed_process::COMMUNICATION_STOPPED)) == "stopped" &&
        std::string(Reader::communication_state_name(sintra::Managed_process::COMMUNICATION_PAUSED)) == "paused" &&
        std::string(Reader::communication_state_name(sintra::Managed_process::COMMUNICATION_RUNNING)) == "running" &&
        std::string(Reader::communication_state_name(-1)) == "unknown",
        k_prefix,
        "communication_state_name() should cover known and unknown states");

    const auto missing_summary = Reader::missing_reader_summary(sintra::invalid_instance_id);
    ok &= sintra::test::assert_true(
        missing_summary.find("condition=missing") != std::string::npos &&
        missing_summary.find("target_process_id=unknown") != std::string::npos &&
        missing_summary.find("communication_state=unreachable") != std::string::npos,
        k_prefix,
        "missing_reader_summary() should describe missing invalid readers without runtime");

    const auto process_iid     = sintra::compose_instance(2, 1);
    const auto process_summary = Reader::missing_reader_summary(process_iid);
    ok &= sintra::test::assert_true(
        process_summary.find("condition=missing") != std::string::npos &&
        process_summary.find(
            std::string("target_process_id=") +
            std::to_string(static_cast<unsigned long long>(process_iid))) != std::string::npos &&
        process_summary.find("communication_state=unreachable") != std::string::npos,
        k_prefix,
        "missing_reader_summary() should include numeric process ids without runtime");

    return ok;
}

bool test_leave_after_init_returns_true_and_resets_state(int argc, char* argv[])
{
    sintra::init(argc, argv);

    const bool result = sintra::leave();
    const auto after  = sintra::detail::s_shutdown_state.load(std::memory_order_acquire);

    return
        sintra::test::assert_true(
            result && after == sintra::detail::shutdown_protocol_state::idle,
            k_prefix,
            "leave() after init() should return true and leave state idle"
        );
}

bool test_leave_rejects_in_handler_context(int argc, char* argv[])
{
    sintra::init(argc, argv);

    bool caught = false;
    tl_in_handler_dispatch = true;
    try {
        (void)sintra::leave();
    }
    catch (const std::logic_error&) {
        caught = true;
    }
    tl_in_handler_dispatch = false;

    sintra::detail::finalize();

    const auto after = sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
    return
        sintra::test::assert_true(
            caught && after == sintra::detail::shutdown_protocol_state::idle,
            k_prefix,
            "leave() should reject handler-dispatch context and leave state idle"
        );
}

bool test_double_init_rejected(int argc, char* argv[])
{
    sintra::init(argc, argv);

    bool caught = false;
    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error&) {
        caught = true;
    }

    sintra::detail::finalize();

    return sintra::test::assert_true(
        caught,
        k_prefix,
        "double init() should throw without matching teardown");
}

bool test_init_finalize_cycle(int argc, char* argv[])
{
    sintra::init(argc, argv);
    sintra::detail::finalize();

    const auto mid_state =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
    if (mid_state != sintra::detail::shutdown_protocol_state::idle) {
        sintra::test::print_test_message(
            k_prefix,
            "state should be idle between cycles");
        return false;
    }

    sintra::init(argc, argv);
    sintra::detail::finalize();

    const auto end_state =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
    return
        sintra::test::assert_true(
            end_state == sintra::detail::shutdown_protocol_state::idle,
            k_prefix,
            "state should be idle after second init/finalize cycle"
        );
}

bool test_teardown_admission_rejects_process_entry_points(int argc, char* argv[])
{
    sintra::init(argc, argv);

    bool ok = true;
    {
        Teardown_admission_guard guard;

        sintra::Spawn_options spawn_options;
        spawn_options.binary_path = (argc > 0 && argv[0] && argv[0][0] != '\0')
            ? argv[0]
            : "lifecycle_guardrails_test";

        ok &= sintra::test::assert_true(
            sintra::spawn_swarm_process(spawn_options) == 0,
            k_prefix,
            "spawn_swarm_process() should reject while teardown admission is closed");

        ok &= sintra::test::assert_true(
            sintra::join_swarm(1) == sintra::invalid_instance_id,
            k_prefix,
            "join_swarm() should reject while teardown admission is closed");

        const auto invitation = sintra::create_external_process_invitation();
        ok &= sintra::test::assert_true(
            !invitation,
            k_prefix,
            "create_external_process_invitation() should reject while teardown admission is closed");
    }

    ok &= sintra::test::assert_true(
        sintra::detail::finalize(),
        k_prefix,
        "finalize() should succeed after teardown admission guard resets state");

    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    bool ok = true;

    ok &= test_finalize_rejects_for_all_active_states();
    ok &= test_shutdown_rejects_for_all_active_states();
    ok &= test_leave_rejects_for_all_active_states();
    ok &= test_barrier_rejects_during_active_teardown();
    ok &= test_shutdown_without_runtime_returns_false();
    ok &= test_leave_without_runtime_returns_false();
    ok &= test_finalize_without_init_returns_false();
    ok &= test_runtime_required_apis_without_runtime_throw();
    ok &= test_runtime_process_entry_early_returns(argc, argv);
    ok &= test_process_message_reader_diagnostic_helpers();
    ok &= test_leave_after_init_returns_true_and_resets_state(argc, argv);
    ok &= test_leave_rejects_in_handler_context(argc, argv);
    ok &= test_state_idle_after_finalize(argc, argv);
    ok &= test_double_init_rejected(argc, argv);
    ok &= test_teardown_admission_rejects_process_entry_points(argc, argv);
    ok &= test_init_finalize_cycle(argc, argv);

    return ok ? 0 : 1;
}
