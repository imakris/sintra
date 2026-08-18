//
// Sintra reader service-mode dispatch policy characterization
//
// READER_SERVICE is the reader state that finalize() enters before it
// deactivates handlers and unpublishes transceivers. This test pins down what
// that state currently admits, at two levels:
//
//   1. The pure predicates of Reader_service_dispatch_policy, which mirror the
//      conditions evaluated by the request and reply reader loops.
//   2. The real runtime, which is used to show that an ordinary user event is
//      still dispatched after Managed_process::pause() in a process that hosts
//      the coordinator.
//
// Both parts are characterization rather than contract: they record what the
// runtime does today, so that a deliberate change of service-mode eligibility
// surfaces as a test failure instead of silent drift. The consequences are
// described in docs/barriers_and_shutdown.md.
//

#include <sintra/sintra.h>
#include <sintra/detail/messaging/process_message_reader.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

// At global scope on purpose: the message type id is derived from the type
// name, and this is an ordinary user payload, which is exactly the kind of
// event whose service-mode treatment is being characterized.
struct service_mode_probe
{
    int value;
};

namespace {

constexpr std::string_view k_prefix =
    "process_reader_service_dispatch_policy_test: ";

using Policy = sintra::detail::Reader_service_dispatch_policy;


bool wait_for_value(const std::atomic<int>& value, int expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load(std::memory_order_acquire) == expected;
}


// The predicates are pure, so the whole current policy can be stated without a
// runtime. The event rule is the interesting one: it is a numeric comparison
// against base_of_messages_handled_by_coordinator, which does not partition
// messages into "runtime" and "application" the way the name suggests.
bool check_policy_predicates()
{
    bool ok = true;

    auto expect = [&](bool condition, std::string_view message) {
        ok &= sintra::test::assert_true(condition, k_prefix, message);
    };

    constexpr auto coordinator     = sintra::compose_instance(2, 2);
    constexpr auto service_target  = sintra::compose_instance(2, 3);
    constexpr auto ordinary_target = sintra::compose_instance(
        2, 2 + sintra::num_reserved_service_instances);
    constexpr auto ordinary_sender = sintra::compose_instance(
        3, 2 + sintra::num_reserved_service_instances);

    constexpr auto coordinator_event =
        static_cast<sintra::type_id_type>(
            sintra::detail::reserved_id::terminated_abnormally);
    constexpr auto coordinator_event_base =
        static_cast<sintra::type_id_type>(
            sintra::detail::reserved_id::
                base_of_messages_handled_by_coordinator);
    constexpr auto generated_event =
        static_cast<sintra::type_id_type>(
            sintra::detail::reserved_id::num_reserved_type_ids) + 1;
    constexpr auto explicit_user_event = sintra::make_user_type_id(1);
    constexpr auto instance_unpublished_event =
        static_cast<sintra::type_id_type>(
            sintra::detail::reserved_id::instance_unpublished);
    constexpr auto unpublish_notify_event =
        static_cast<sintra::type_id_type>(
            sintra::detail::reserved_id::unpublish_transceiver_notify);

    // Events.
    expect(
        !Policy::allow_event(false, coordinator_event),
        "service-mode event dispatch requires a local coordinator");
    expect(
        !Policy::allow_event(true, coordinator_event_base),
        "the coordinator-event threshold itself is excluded");
    expect(
        Policy::allow_event(true, coordinator_event),
        "the reserved coordinator event is admitted");

    // The two directions in which the numeric rule does not match the name
    // "messages handled by coordinator". Both are recorded deliberately.
    expect(
        Policy::allow_event(true, generated_event),
        "characterization: auto-generated user message ids pass the threshold");
    expect(
        Policy::allow_event(true, explicit_user_event),
        "characterization: explicit user message ids pass the threshold");
    expect(
        !Policy::allow_event(true, instance_unpublished_event),
        "characterization: instance_unpublished is below the threshold and is "
        "suppressed in service mode");
    expect(
        !Policy::allow_event(true, unpublish_notify_event),
        "characterization: unpublish_transceiver_notify is below the threshold "
        "and is suppressed in service mode");

    // Targeted requests.
    expect(
        Policy::allow_targeted_request(
            true, service_target, ordinary_sender, coordinator),
        "a service target is admitted when the coordinator is local");
    expect(
        !Policy::allow_targeted_request(
            false, service_target, ordinary_sender, coordinator),
        "a service target alone is insufficient without a local coordinator");
    expect(
        Policy::allow_targeted_request(
            false, ordinary_target, coordinator, coordinator),
        "a coordinator-sent targeted request is admitted in every process");
    expect(
        !Policy::allow_targeted_request(
            true, ordinary_target, ordinary_sender, coordinator),
        "an ordinary targeted request is denied in service mode");

    // Replies.
    expect(
        Policy::allow_reply(
            true, coordinator, ordinary_sender, coordinator),
        "a reply addressed to the local coordinator is admitted");
    expect(
        !Policy::allow_reply(
            false, coordinator, ordinary_sender, coordinator),
        "reply-to-coordinator admission requires the coordinator object locally");
    expect(
        Policy::allow_reply(
            false, ordinary_target, coordinator, coordinator),
        "a reply sent by the coordinator is admitted in every process");
    expect(
        !Policy::allow_reply(
            true, ordinary_target, ordinary_sender, coordinator),
        "an ordinary reply is denied in service mode");

    return ok;
}


// pause() publishes READER_SERVICE into every reader, but it does not wake and
// acknowledge a reader that is already blocked inside fetch_message() after
// sampling READER_NORMAL. A single event emitted right after pause() can
// therefore still be consumed by the pre-pause iteration, which is why this
// test does not draw any conclusion from the first post-pause event.
//
// Instead it emits two events in sequence and waits for the first to be
// handled before emitting the second. A reader iteration handles exactly one
// message, so the second event cannot be consumed before the iteration that
// handled the first has finished. That next iteration samples m_reader_state
// after pause() returned, so it necessarily observes READER_SERVICE.
bool characterize_runtime_service_mode_dispatch(int argc, char* argv[])
{
    bool ok = true;

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& exception) {
        return sintra::test::assert_true(
            false,
            k_prefix,
            std::string("runtime characterization init failed: ") +
                exception.what());
    }

    std::atomic<int> last_seen{0};
    auto deactivate = sintra::activate_slot(
        [&last_seen](service_mode_probe probe) {
            last_seen.store(probe.value, std::memory_order_release);
        });

    sintra::local() << service_mode_probe{1};
    ok &= sintra::test::assert_true(
        wait_for_value(last_seen, 1),
        k_prefix,
        "an ordinary user event must dispatch before pause()");

    ok &= sintra::test::assert_true(
        sintra::s_coord != nullptr,
        k_prefix,
        "runtime characterization requires a local coordinator");

    if (ok && sintra::s_mproc && sintra::s_coord) {
        sintra::s_mproc->pause();

        ok &= sintra::test::assert_true(
            sintra::s_mproc->communication_state() ==
                sintra::Managed_process::COMMUNICATION_PAUSED,
            k_prefix,
            "pause() must publish the paused communication state");

        // Step 1: retire whichever reader iteration may still be carrying a
        // pre-pause READER_NORMAL sample.
        sintra::local() << service_mode_probe{2};
        ok &= sintra::test::assert_true(
            wait_for_value(last_seen, 2),
            k_prefix,
            "the reader must advance past its pre-pause iteration");

        // Step 2: this event can only be consumed by an iteration that sampled
        // the reader state after pause(), that is, READER_SERVICE.
        sintra::local() << service_mode_probe{3};
        ok &= sintra::test::assert_true(
            wait_for_value(last_seen, 3),
            k_prefix,
            "characterization: an ordinary user event is still dispatched in "
            "coordinator service mode");
    }

    ok &= sintra::test::assert_true(
        sintra::detail::finalize(),
        k_prefix,
        "runtime characterization finalization should complete");

    (void)deactivate;
    return ok;
}

} // namespace


int main(int argc, char* argv[])
{
    bool ok = check_policy_predicates();
    ok &= characterize_runtime_service_mode_dispatch(argc, argv);
    return ok ? 0 : 1;
}
