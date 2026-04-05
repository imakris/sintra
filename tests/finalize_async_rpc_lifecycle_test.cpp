#include <sintra/sintra.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {

struct Finalize_async_bus : sintra::Derived_transceiver<Finalize_async_bus>
{
    int delayed_reply(int value, int delay_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        return value;
    }

    SINTRA_RPC_STRICT(delayed_reply)
};

} // namespace

int main(int argc, char* argv[])
{
    using namespace std::chrono_literals;

    try {
        sintra::init(argc, const_cast<const char* const*>(argv));
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to initialize sintra: %s\n", e.what());
        return 1;
    }

    Finalize_async_bus bus;

    auto handle = Finalize_async_bus::rpc_async_delayed_reply(
        bus.instance_id(),
        515,
        500);

    if (handle.wait_until(std::chrono::steady_clock::now() + 40ms) !=
        sintra::Rpc_wait_status::deadline_exceeded)
    {
        std::fprintf(stderr, "Async RPC completed before finalize began.\n");
        return 1;
    }

    sintra::finalize();

    if (!handle.ready()) {
        std::fprintf(stderr, "Handle was still pending after finalize.\n");
        return 1;
    }

    if (handle.state() != sintra::Rpc_completion_state::cancelled) {
        std::fprintf(stderr, "Handle did not transition to cancelled after finalize.\n");
        return 1;
    }

    try {
        (void)handle.get();
        std::fprintf(stderr, "Handle returned a value after finalize.\n");
        return 1;
    }
    catch (const sintra::rpc_cancelled&) {
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Handle threw unexpected exception after finalize: %s\n", e.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "Handle threw non-std exception after finalize.\n");
        return 1;
    }

    return 0;
}
