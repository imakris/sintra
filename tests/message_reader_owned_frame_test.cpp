#include <sintra/detail/messaging/message.h>
#include <sintra/detail/messaging/message_impl.h>

#include "test_ring_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view k_prefix = "message_reader_owned_frame_test: ";

struct alignas(16) aligned_value_t
{
    std::array<uint64_t, 2> words;
};

struct payload_t
{
    uint64_t sequence;
    sintra::message_string text;
    sintra::typed_variable_buffer<std::vector<aligned_value_t>> values;
};

using Test_message = sintra::Message<payload_t, void, 0x71A4E01ull>;

struct Dispatch_bus : sintra::Derived_transceiver<Dispatch_bus>
{
    SINTRA_MESSAGE(Frame, uint64_t sequence, sintra::message_string text);

    void send(uint64_t sequence, const std::string& text)
    {
        emit_local<Frame>(sequence, text);
    }
};

void write_message(
    sintra::Message_ring_W& writer,
    uint64_t sequence,
    const std::string& text,
    const std::vector<aligned_value_t>& values)
{
    auto* message = writer.write<Test_message>(
        sintra::vb_size<Test_message>(sequence, text, values), sequence, text, values);
    message->sender_instance_id = 123;
    message->function_instance_id = 456;
    writer.done_writing();
}

bool verify_payload(const Test_message& message, uint64_t sequence, const std::string& text)
{
    const auto values = static_cast<std::vector<aligned_value_t>>(message.values);
    return sintra::test::assert_true(
        message.sequence == sequence && static_cast<std::string>(message.text) == text &&
        values.size() == 2 && values[0].words[0] == 17 && values[1].words[1] == 29 &&
        message.sender_instance_id == 123 && message.function_instance_id == 456 &&
        reinterpret_cast<uintptr_t>(message.values.data_address()) % alignof(aligned_value_t) == 0,
        k_prefix, "owned frame must preserve aligned variable payloads and reply routing");
}

class Eviction_log
{
public:
    Eviction_log()
    {
        m_previous = sintra::get_log_callback(&m_previous_data);
        sintra::set_log_callback(&capture, this);
    }

    ~Eviction_log() { sintra::set_log_callback(m_previous, m_previous_data); }

    std::atomic<unsigned> m_count{0};

private:
    static void capture(sintra::log_level level, const char* message, void* data)
    {
        if (level == sintra::log_level::warning && std::string_view(message).find("evict") !=
            std::string_view::npos)
        {
            static_cast<Eviction_log*>(data)->m_count.fetch_add(1);
        }
    }

    sintra::log_callback_fn m_previous;
    void* m_previous_data = nullptr;
};

bool run_test()
{
    using namespace std::chrono_literals;
    sintra::test::Temp_ring_dir directory("message_reader_owned_frame");
    sintra::Message_ring_W writer(directory.str(), "req", 1);
    sintra::Message_ring_R reader(directory.str(), "req", 1);
    sintra::detail::Instance_name_cache names;
    constexpr auto old_instance = sintra::compose_instance(4, 11);
    names.resolve("discarded-publication", [](const std::string&) { return old_instance; });
    std::atomic<bool> loss_reported{false};
    reader.set_eviction_handler([&]() {
        names.clear();
        loss_reported = true;
    });
    reader.start_reading();
    const std::vector<aligned_value_t> values = {{{17, 19}}, {{23, 29}}};
    const std::string original = "frame retained through dispatch";
    write_message(writer, 1, original, values);
    const auto first_sequence = writer.get_leading_sequence();
    write_message(writer, 2, "unread frame", values);

    auto* held = static_cast<Test_message*>(reader.fetch_message());
    const auto address = reinterpret_cast<uintptr_t>(held);
    const auto mapped = reinterpret_cast<uintptr_t>(reader.get_base_address());
    if (!sintra::test::assert_true(
            held && (address < mapped || address >= mapped + 2 * sintra::message_ring_size),
            k_prefix, "dispatch must use owned storage outside the writer's ring mapping"))
    {
        return false;
    }
    bool ok = verify_payload(*held, 1, original);
    ok &= sintra::test::assert_true(reader.get_message_reading_sequence() == first_sequence,
        k_prefix, "fetch position must advance one frame, not the whole batch");

    sintra::Message_ring_W relay_writer(directory.str(), "req", 2);
    sintra::Message_ring_R relay_reader(directory.str(), "req", 2);
    relay_reader.start_reading();
    relay_writer.relay(*held);
    ok &= verify_payload(*static_cast<Test_message*>(relay_reader.fetch_message()), 1, original);

    Eviction_log eviction_log;
    const std::string filler(32768, 'f');
    const auto before_laps = writer.get_leading_sequence();
    while (writer.get_leading_sequence() - before_laps < 2 * sintra::message_ring_size) {
        write_message(writer, 3, filler, values);
    }
    ok &= sintra::test::assert_true(writer.get_diagnostics().reader_eviction_count > 0,
        k_prefix, "producer must progress past a retained dispatch frame");
    ok &= verify_payload(*held, 1, original);

    const auto resume_sequence = writer.get_leading_sequence();
    std::atomic<bool> fetch_finished{false};
    Test_message* next = nullptr;
    std::exception_ptr fetch_error;
    std::thread fetcher([&]() {
        try {
            next = static_cast<Test_message*>(reader.fetch_message());
        }
        catch (...) {
            fetch_error = std::current_exception();
        }
        fetch_finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!loss_reported && !fetch_finished &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    const bool resumed = loss_reported &&
        reader.get_message_reading_sequence() == resume_sequence;
    ok &= sintra::test::assert_true(resumed && !fetch_finished &&
        names.resolve("discarded-publication", [](const std::string&) {
            return sintra::invalid_instance_id;
        }) == sintra::invalid_instance_id,
        k_prefix, "loss must clear cached names before fetch waits for new traffic");
    if (resumed) {
        write_message(writer, 4, "after resynchronization", values);
    }
    else {
        reader.request_stop();
    }
    fetcher.join();
    ok &= sintra::test::assert_true(resumed && !fetch_error && next,
        k_prefix, "evicted unread frames must be discarded before parsing resumes");
    if (resumed && !fetch_error && next) {
        ok &= verify_payload(*next, 4, "after resynchronization");
    }
    ok &= sintra::test::assert_true(eviction_log.m_count.load() == 1,
        k_prefix, "one observed eviction must produce one warning");
    reader.done_reading();
    relay_reader.done_reading();
    return ok;
}

bool run_dispatch_test(int argc, char* argv[])
{
    using namespace std::chrono_literals;
    sintra::init(argc, argv);
    bool ok = true;
    {
        Dispatch_bus bus;
        const std::string original = "live callback keeps its complete frame";
        const std::string filler(32768, 'f');
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        std::atomic<bool> payload_preserved{false};
        std::atomic<bool> post_finished{false};
        auto deactivate = sintra::activate_slot([&](const Dispatch_bus::Frame& message) {
            if (message.sequence != 1) {
                return;
            }
            entered = true;
            release.wait(false);
            payload_preserved = static_cast<std::string>(message.text) == original;
            bus.send(4, "emit from the current handler");
            sintra::s_mproc->run_after_current_handler([&]() {
                bus.send(5, "emit from the current post-handler");
                post_finished = true;
            });
        });
        struct Dispatch_cleanup
        {
            std::atomic<bool>& m_release;
            sintra::Transceiver::handler_deactivator& m_deactivate;
            ~Dispatch_cleanup()
            {
                m_release = true;
                m_release.notify_all();
                m_deactivate();
            }
        } cleanup{release, deactivate};

        bus.send(1, original);
        const auto entered_deadline = std::chrono::steady_clock::now() + 5s;
        while (!entered && std::chrono::steady_clock::now() < entered_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        if (!sintra::test::assert_true(entered.load(), k_prefix, "real slot must start dispatch")) {
            std::_Exit(1);
        }
        if (entered) {
            const auto initial_sequence = sintra::s_mproc->m_out_req_c->get_leading_sequence();
            while (sintra::s_mproc->m_out_req_c->get_leading_sequence() - initial_sequence <
                2 * sintra::message_ring_size)
            {
                bus.send(3, filler);
            }
        }
        release = true;
        release.notify_all();
        const auto completed_deadline = std::chrono::steady_clock::now() + 5s;
        while (entered && !post_finished && std::chrono::steady_clock::now() < completed_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        if (entered && !post_finished) {
            std::fprintf(stderr, "%.*sdispatch did not release transport locks before emitting\n",
                int(k_prefix.size()), k_prefix.data());
            std::_Exit(1);
        }
        ok &= sintra::test::assert_true(payload_preserved.load(), k_prefix,
            "callback payload must remain unchanged while its producer laps the ring");
    }
    sintra::shutdown();
    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        return run_test() && run_dispatch_test(argc, argv) ? 0 : 1;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%.*s%s\n", int(k_prefix.size()), k_prefix.data(), e.what());
        return 1;
    }
}
