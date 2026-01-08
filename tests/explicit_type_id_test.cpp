#include <sintra/sintra.h>

namespace {

constexpr sintra::type_id_type k_bus_id = 0x120;
constexpr sintra::type_id_type k_message_id = 0x121;

struct Explicit_bus : sintra::Derived_transceiver<Explicit_bus>
{
    SINTRA_TYPE_ID(k_bus_id)
    SINTRA_MESSAGE_EXPLICIT(ping, k_message_id, int value)
};

static_assert(
    Explicit_bus::sintra_type_id() == sintra::make_user_type_id(k_bus_id));
static_assert(
    Explicit_bus::ping::sintra_type_id() == sintra::make_user_type_id(k_message_id));

int run_checks()
{
    const auto expected_message_id = sintra::make_user_type_id(k_message_id);
    const auto actual_message_id = Explicit_bus::ping::id();
    if (actual_message_id != expected_message_id) {
        return 1;
    }
    if (!sintra::is_user_type_id(actual_message_id)) {
        return 2;
    }

    const auto expected_bus_id = sintra::make_user_type_id(k_bus_id);
    const auto actual_bus_id = sintra::get_type_id<Explicit_bus>();
    if (actual_bus_id != expected_bus_id) {
        return 3;
    }
    if (!sintra::is_user_type_id(actual_bus_id)) {
        return 4;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);
    const int result = run_checks();
    sintra::finalize();
    return result;
}
