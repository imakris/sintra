//
// Sintra library, example 7
//
// This example shows optional explicit type ids for transceivers and messages.
// Most users can rely on the default type ids as long as every process is built
// with the same toolchain. Use explicit ids when mixing toolchains or when you
// want deterministic ids across builds.
//
#include <sintra/sintra.h>
#include <condition_variable>
#include <mutex>

namespace {

// Choose unique, stable values within your codebase; keep them consistent
// across builds and toolchains when explicit ids are required.
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
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    bool received = false;

    auto slot = [](const Explicit_bus::ping& msg) {
        sintra::console() << "ping value=" << msg.value << '\n';
    };

    auto notified_slot = [&](const Explicit_bus::ping& msg) {
        slot(msg);
        {
            std::lock_guard<std::mutex> lock(wait_mutex);
            received = true;
        }
        wait_cv.notify_one();
    };

    sintra::activate_slot(notified_slot);
    bus.emit_global<Explicit_bus::ping>(42);

    {
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_cv.wait(lock, [&] { return received; });
    }

    sintra::finalize();
    return 0;
}
