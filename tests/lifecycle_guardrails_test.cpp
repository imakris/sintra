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
// 9. Double init() without teardown is rejected.
// 10. init()/finalize() cycles work correctly (state resets between cycles).

#include <sintra/sintra.h>

#include "test_utils.h"

#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view k_prefix = "lifecycle_guardrails_test: ";

struct state_case_t {
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
    sintra::detail::s_shutdown_state.store(
        sintra::detail::shutdown_protocol_state::idle,
        std::memory_order_release);
}

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

    const bool result = sintra::leave();
    const auto state_after =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);

    return sintra::test::assert_true(
        !result &&
            state_after == sintra::detail::shutdown_protocol_state::idle,
        k_prefix,
        "leave() without init() should return false and leave state idle");
}

bool test_shutdown_without_runtime_returns_false()
{
    reset_shutdown_state();

    const bool result = sintra::shutdown();
    const auto state_after =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);

    return sintra::test::assert_true(
        !result &&
            state_after == sintra::detail::shutdown_protocol_state::idle,
        k_prefix,
        "shutdown() without init() should return false and leave state idle");
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
    return sintra::test::assert_true(
        after == sintra::detail::shutdown_protocol_state::idle,
        k_prefix,
        "state should be idle after finalize");
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

bool test_leave_after_init_returns_true_and_resets_state(int argc, char* argv[])
{
    sintra::init(argc, argv);

    const bool result = sintra::leave();
    const auto after = sintra::detail::s_shutdown_state.load(std::memory_order_acquire);

    return sintra::test::assert_true(
        result &&
            after == sintra::detail::shutdown_protocol_state::idle,
        k_prefix,
        "leave() after init() should return true and leave state idle");
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
    return sintra::test::assert_true(
        end_state == sintra::detail::shutdown_protocol_state::idle,
        k_prefix,
        "state should be idle after second init/finalize cycle");
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
    ok &= test_leave_after_init_returns_true_and_resets_state(argc, argv);
    ok &= test_state_idle_after_finalize(argc, argv);
    ok &= test_double_init_rejected(argc, argv);
    ok &= test_init_finalize_cycle(argc, argv);

    return ok ? 0 : 1;
}
