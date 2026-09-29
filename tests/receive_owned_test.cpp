#include <sintra/sintra.h>

#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

constexpr std::string_view k_prefix = "receive_owned_test: ";

struct alignas(16) aligned_value_t
{
    std::array<uint64_t, 2> words;
};

using Aligned_values = std::vector<aligned_value_t>;

struct Receive_bus : sintra::Derived_transceiver<Receive_bus>
{
    SINTRA_MESSAGE(Frame, unsigned sequence, sintra::message_string text,
        sintra::typed_variable_buffer<Aligned_values> values);
    SINTRA_MESSAGE(Fixed, unsigned sequence);

    void send(unsigned sequence, const std::string& text, const Aligned_values& values)
    {
        emit_local<Frame>(sequence, text, values);
    }
};

static_assert(!sintra::detail::owned_message_supported<std::string>::value);
static_assert(!std::is_copy_constructible_v<sintra::Owned_message<Receive_bus::Frame>>);

template <typename Predicate>
void wait_for(Predicate predicate, const char* failure)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "%.*s%s\n", int(k_prefix.size()), k_prefix.data(), failure);
            std::_Exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

template <typename T>
size_t slot_count()
{
    std::lock_guard<std::recursive_mutex> lock(sintra::s_mproc->m_handlers_mutex);
    const auto found = sintra::s_mproc->m_active_handlers.find(T::id());
    size_t count = 0;
    if (found != sintra::s_mproc->m_active_handlers.end()) {
        for (const auto& entry : found->second) {
            count += entry.second.size();
        }
    }
    return count;
}

void verify(const Receive_bus::Frame& message)
{
    const auto values = static_cast<Aligned_values>(message.values);
    sintra::test::require_true(
        message.sequence == 17 && static_cast<std::string>(message.text) == "retained frame" &&
        values.size() == 2 && values[0].words[0] == 13 && values[1].words[1] == 29 &&
        reinterpret_cast<uintptr_t>(message.values.data_address()) % alignof(aligned_value_t) == 0,
        k_prefix, "owned message lost its independent aligned payload");
}

void run_lifetime_test()
{
    Receive_bus sender;
    Receive_bus other_sender;
    const Aligned_values values = {{{13, 19}}, {{23, 29}}};
    sintra::Owned_message<Receive_bus::Frame> result;
    std::atomic<bool> completed{false};
    std::thread receiver([&]() {
        result = sintra::receive_owned<Receive_bus::Frame>(
            sintra::Typed_instance_id<Receive_bus>(sender));
        completed = true;
    });
    wait_for([] { return slot_count<Receive_bus::Frame>() == 1; }, "receive slot was not installed");

    std::atomic<unsigned> observed{0};
    std::atomic<bool> release_handler{false};
    auto deactivate = sintra::activate_slot([&](const Receive_bus::Frame& message) {
        observed = message.sequence;
        if (message.sequence == 17) {
            release_handler.wait(false);
            verify(message);
        }
    });
    other_sender.send(11, "wrong sender", {});
    wait_for([&] { return observed == 11; }, "wrong-sender frame was not dispatched");
    sintra::test::require_true(!completed, k_prefix, "sender filter accepted another sender");
    sender.send(17, "retained frame", values);
    wait_for([&] { return completed.load(); }, "owned receive did not complete");
    receiver.join();
    wait_for([&] { return observed == 17; }, "other matching handler did not run");
    verify(*result);
    auto* address = result.get();
    auto moved = std::move(result);
    sintra::test::require_true(!result && moved.get() == address, k_prefix,
        "moving the owner must keep the frame address stable");
    release_handler = true;
    release_handler.notify_all();
    sender.send(31, std::string(16384, 'r'), values);
    wait_for([&] { return observed == 31; }, "dispatch storage was not reused");
    verify(*moved);
    deactivate();

    completed = false;
    std::thread empty_receiver([&]() {
        result = sintra::receive_owned<Receive_bus::Frame>();
        completed = true;
    });
    wait_for([] { return slot_count<Receive_bus::Frame>() == 1; }, "empty receive slot missing");
    other_sender.send(37, "", {});
    wait_for([&] { return completed.load(); }, "empty receive did not complete");
    empty_receiver.join();
    sintra::test::require_true(result->sequence == 37 &&
        static_cast<std::string>(result->text).empty() &&
        static_cast<Aligned_values>(result->values).empty(), k_prefix, "empty payload changed");
    result.reset();
    verify(*moved);
}

void run_fixed_and_error_test()
{
    Receive_bus sender;
    sintra::Owned_message<Receive_bus::Fixed> result;
    std::atomic<bool> completed{false};
    std::thread receiver([&]() {
        result = sintra::receive_owned<Receive_bus::Fixed>();
        completed = true;
    });
    wait_for([] { return slot_count<Receive_bus::Fixed>() == 1; }, "fixed receive slot missing");
    sender.emit_local<Receive_bus::Fixed>(53u);
    wait_for([&] { return completed.load(); }, "fixed receive did not complete");
    receiver.join();
    sintra::test::require_true(result->sequence == 53, k_prefix, "fixed payload changed");

    // A callback-side copy failure must wake the caller and remove its slot.
    completed = false;
    std::atomic<bool> caught{false};
    std::thread failing_receiver([&]() {
        try {
            (void)sintra::receive_owned<Receive_bus::Fixed>();
        }
        catch (const sintra::corrupted_message_exception&) {
            caught = true;
        }
        completed = true;
    });
    wait_for([] { return slot_count<Receive_bus::Fixed>() == 1; }, "error receive slot missing");
    Receive_bus::Fixed malformed(59u);
    malformed.bytes_to_next_message = sizeof(Receive_bus::Fixed) - 1;
    sintra::dispatch_event_handlers(malformed, {sintra::any_local_or_remote});
    wait_for([&] { return completed.load(); }, "copy error did not wake receiver");
    failing_receiver.join();
    sintra::test::require_true(caught && slot_count<Receive_bus::Fixed>() == 0,
        k_prefix, "copy error was not propagated after deactivation");
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);
    run_lifetime_test();
    run_fixed_and_error_test();
    sintra::shutdown();
    std::fprintf(stderr, "receive_owned_test passed\n");
}
