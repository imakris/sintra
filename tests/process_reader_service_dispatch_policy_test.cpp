#include <sintra/sintra.h>
#include <sintra/detail/messaging/process_message_reader.h>

#include "test_utils.h"

#include <string_view>

namespace {

constexpr std::string_view k_prefix =
    "process_reader_service_dispatch_policy_test: ";

using Policy = sintra::detail::Reader_service_dispatch_policy;

} // namespace

int main()
{
    bool ok = true;

    constexpr auto coordinator = sintra::compose_instance(2, 2);
    constexpr auto service_target = sintra::compose_instance(2, 3);
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

    auto expect = [&](bool condition, std::string_view message) {
        ok &= sintra::test::assert_true(condition, k_prefix, message);
    };

    expect(
        !Policy::allow_event(false, coordinator_event),
        "service-mode event dispatch requires a local coordinator");
    expect(
        !Policy::allow_event(true, coordinator_event_base),
        "the coordinator-event threshold itself is excluded");
    expect(
        Policy::allow_event(true, coordinator_event),
        "the reserved coordinator event is admitted");
    expect(
        Policy::allow_event(true, generated_event),
        "characterization: auto-generated user message ids pass the threshold");
    expect(
        Policy::allow_event(true, explicit_user_event),
        "characterization: explicit user message ids pass the threshold");

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

    return ok ? 0 : 1;
}
