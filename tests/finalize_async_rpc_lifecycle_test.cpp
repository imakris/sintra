#include <sintra/sintra.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace {

struct Reply_gate
{
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    bool timed_out = false;

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct Finalize_async_bus : sintra::Derived_transceiver<Finalize_async_bus>
{
    explicit Finalize_async_bus(Reply_gate& gate) : m_gate(gate) {}

    int delayed_reply(int value)
    {
        std::unique_lock<std::mutex> lock(m_gate.mutex);
        m_gate.entered = true;
        m_gate.cv.notify_all();
        m_gate.timed_out = !m_gate.cv.wait_for(
            lock, std::chrono::seconds(10), [&] { return m_gate.released; });
        return value;
    }

    SINTRA_RPC_STRICT(delayed_reply)

    Reply_gate& m_gate;
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

    Reply_gate gate;
    Finalize_async_bus bus(gate);

    auto handle = Finalize_async_bus::rpc_async_delayed_reply(
        bus.instance_id(),
        515);

    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (!gate.cv.wait_for(lock, 5s, [&] { return gate.entered; })) {
            std::fprintf(stderr, "RPC handler did not enter before finalize.\n");
            lock.unlock();
            gate.release();
            sintra::detail::finalize();
            return 1;
        }
    }

    bool cancelled = false;
    // Finalize cancels pending handles before waiting for active handlers.
    // Release this handler only after its caller observes that cancellation.
    std::thread observer([&] {
        try {
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
            (void)handle.get(std::stop_token{});
#else
            (void)handle.get_until(std::chrono::steady_clock::now() + 5s);
#endif
            std::fprintf(stderr, "Handle returned a value during finalize.\n");
        }
        catch (const sintra::rpc_cancelled&) {
            cancelled = true;
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "Handle threw unexpected exception during finalize: %s\n", e.what());
        }
        catch (...) {
            std::fprintf(stderr, "Handle threw non-std exception during finalize.\n");
        }
        gate.release();
    });

    const bool finalized = sintra::detail::finalize();
    observer.join();
    if (!finalized || !cancelled || gate.timed_out) {
        std::fprintf(stderr, "Finalize did not cancel the pending RPC while its handler was held.\n");
        return 1;
    }

    return 0;
}
