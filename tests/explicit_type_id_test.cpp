#include <sintra/sintra.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace {

constexpr sintra::type_id_type k_bus_id     = 0x120;
constexpr sintra::type_id_type k_message_id = 0x121;

struct Explicit_bus : sintra::Derived_transceiver<Explicit_bus>
{
    SINTRA_TYPE_ID(k_bus_id)
    SINTRA_MESSAGE_EXPLICIT(ping, k_message_id, int value)
};

struct Conflicting_bus
{
    SINTRA_TYPE_ID(k_bus_id)
};

static_assert(
    Explicit_bus::sintra_type_id() == sintra::make_user_type_id(k_bus_id));
static_assert(
    Explicit_bus::ping::sintra_type_id() == sintra::make_user_type_id(k_message_id));

int run_checks()
{
    const auto expected_message_id = sintra::make_user_type_id(k_message_id);
    const auto actual_message_id   = Explicit_bus::ping::id();
    if (actual_message_id != expected_message_id)    { return 1; }
    if (!sintra::is_user_type_id(actual_message_id)) { return 2; }

    const auto registration_start = std::chrono::steady_clock::now();
    const auto expected_bus_id = sintra::make_user_type_id(k_bus_id);
    const auto actual_bus_id   = sintra::get_type_id<Explicit_bus>();
    const auto registration_time = std::chrono::steady_clock::now() - registration_start;
    if (actual_bus_id != expected_bus_id)        { return 3; }
    if (!sintra::is_user_type_id(actual_bus_id)) { return 4; }
    if (registration_time >= std::chrono::milliseconds(1500)) {
        std::fprintf(stderr, "Explicit type registration stalled for %.0f ms\n",
            std::chrono::duration<double, std::milli>(registration_time).count());
        return 5;
    }
    if (sintra::get_type_id<Explicit_bus>() != actual_bus_id) { return 6; }

    try {
        sintra::get_type_id<Conflicting_bus>();
        std::fprintf(stderr, "Explicit type ID collision was accepted\n");
        return 7;
    }
    catch (const std::runtime_error&) {
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);
    const int result = run_checks();
    sintra::shutdown();
    return result;
}
