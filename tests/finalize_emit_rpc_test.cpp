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

    SINTRA_RPC(rpc_noop)
};

} // namespace

int main(int argc, char* argv[])
{
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
    sintra::finalize();

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
