#include <sintra/sintra.h>

#include <cstdio>
#include <stdexcept>

namespace {

struct Finalize_bus : sintra::Derived_transceiver<Finalize_bus>
{
    SINTRA_MESSAGE(ping, int value);

    int rpc_noop()
    {
        return 42;
    }

    void unicast_value(int value)
    {
        received_value = value;
    }

    SINTRA_RPC(rpc_noop)
    SINTRA_UNICAST(unicast_value)

    int received_value = 0;
};

bool unicast_throws_without_runtime(sintra::instance_id_type target, const char* phase)
{
    try {
        Finalize_bus::rpc_unicast_value(target, 7);
    }
    catch (const std::runtime_error&) {
        return true;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Unicast threw unexpected exception %s: %s\n", phase, e.what());
        return false;
    }
    catch (...) {
        std::fprintf(stderr, "Unicast threw non-std exception %s.\n", phase);
        return false;
    }

    std::fprintf(stderr, "Unicast did not throw %s.\n", phase);
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    if (!unicast_throws_without_runtime(sintra::invalid_instance_id, "before init")) {
        return 1;
    }

    try {
        sintra::init(argc, const_cast<const char* const*>(argv));
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to initialize sintra: %s\n", e.what());
        return 1;
    }

    Finalize_bus bus;
    if (!bus.assign_name("finalize_bus")) {
        std::fprintf(stderr, "Failed to assign transceiver name.\n");
        return 1;
    }
    const auto target = bus.instance_id();
    Finalize_bus::rpc_unicast_value(target, 73);
    if (bus.received_value != 73) {
        std::fprintf(stderr, "Unicast did not reach the live local target.\n");
        return 1;
    }
    sintra::detail::finalize();

    if (!unicast_throws_without_runtime(target, "after finalize")) {
        return 1;
    }

    try {
        bus.emit_remote<Finalize_bus::ping>(1);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "emit_remote threw after finalize: %s\n", e.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "emit_remote threw non-std exception after finalize.\n");
        return 1;
    }

    bool threw = false;
    try {
        Finalize_bus::rpc_rpc_noop(sintra::invalid_instance_id);
    }
    catch (const std::runtime_error&) {
        threw = true;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "RPC threw unexpected exception: %s\n", e.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "RPC threw non-std exception.\n");
        return 1;
    }

    if (!threw) {
        std::fprintf(stderr, "RPC did not throw after finalize.\n");
        return 1;
    }

    return 0;
}
