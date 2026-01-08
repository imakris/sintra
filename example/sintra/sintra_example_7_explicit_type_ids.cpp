//
// Sintra library, example 7
//
// This example shows optional explicit type ids for transceivers and messages.
// Most users can rely on the default type ids as long as every process is built
// with the same toolchain. Use explicit ids when mixing toolchains or when you
// want deterministic ids across builds.
//
#include <sintra/sintra.h>
#include <chrono>
#include <thread>

namespace {

constexpr sintra::type_id_type k_bus_id = 0x120;
constexpr sintra::type_id_type k_ping_id = 0x121;

struct Explicit_bus : sintra::Derived_transceiver<Explicit_bus>
{
    SINTRA_TYPE_ID(k_bus_id)
    SINTRA_MESSAGE_EXPLICIT(ping, k_ping_id, int value)
};

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    Explicit_bus bus;

    auto slot = [](const Explicit_bus::ping& msg) {
        sintra::console() << "ping value=" << msg.value << '\n';
    };

    sintra::activate_slot(slot);
    bus.emit_global<Explicit_bus::ping>(42);

    sintra::barrier<sintra::processing_fence_t>(
        "explicit-id-example", "_sintra_all_processes");

    sintra::finalize();
    return 0;
}
