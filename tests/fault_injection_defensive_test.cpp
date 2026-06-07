// Fault-injection test for defensive error paths.

#include <sintra/detail/id_types.h>
#include <sintra/detail/messaging/message.h>
#include <sintra/detail/messaging/message_impl.h>

#include "test_utils.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view k_prefix = "fault_injection_defensive_test: ";

template <typename T>
struct Fake_container
{
    using iterator = typename std::vector<T>::const_iterator;

    size_t   size()  const { return count;           }
    iterator begin() const { return backing.begin(); }
    iterator end()   const { return backing.end();   }

    size_t         count = 0;
    std::vector<T> backing;
};

struct Throwing_variable_buffer_payload
{
    sintra::typed_variable_buffer<std::vector<uint8_t>> bytes;

    Throwing_variable_buffer_payload() = default;

    explicit Throwing_variable_buffer_payload(const std::vector<uint8_t>& value)
        : bytes(value)
    {
        throw std::runtime_error("intentional variable_buffer construction failure");
    }
};

using Throwing_variable_buffer_message =
    sintra::Message<Throwing_variable_buffer_payload, void, 0xE7707ull>;

struct Large_variable_buffer_payload
{
    sintra::typed_variable_buffer<std::vector<uint8_t>> bytes;
};

using Large_variable_buffer_message =
    sintra::Message<Large_variable_buffer_payload, void, 0xE7708ull>;

template <typename Ex, typename Fn>
bool expect_throw(std::string_view label, Fn&& fn)
{
    try {
        fn();
    }
    catch (const Ex&) {
        return true;
    }
    catch (const std::exception& e) {
        sintra::test::print_test_message(
            k_prefix,
            std::string(label) + " threw unexpected exception: " + e.what());
        return false;
    }
    catch (...) {
        sintra::test::print_test_message(
            k_prefix,
            std::string(label) + " threw unknown exception");
        return false;
    }

    sintra::test::print_test_message(k_prefix, std::string(label) + " did not throw");
    return false;
}

bool test_variable_buffer_container_too_large()
{
    Fake_container<uint64_t> container;
    container.count = (std::numeric_limits<size_t>::max() / sizeof(uint64_t)) + 1;
    return expect_throw<std::runtime_error>(
        "variable_buffer container too large",
        [&]() { sintra::variable_buffer vb(container); });
}

bool test_variable_buffer_payload_too_large()
{
    Fake_container<uint8_t> container;
    container.count = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1;
    return expect_throw<std::runtime_error>(
        "variable_buffer payload exceeds 32-bit limit",
        [&]() { sintra::variable_buffer vb(container); });
}

bool test_variable_buffer_requires_message_context()
{
    std::vector<uint8_t> payload{1, 2, 3};
    return expect_throw<std::runtime_error>(
        "variable_buffer requires message construction context",
        [&]() { sintra::variable_buffer vb(payload); });
}

bool test_variable_buffer_context_restored_after_throw()
{
    std::vector<uint8_t> payload{1, 2, 3};
    const size_t         extra_bytes =
        sintra::vb_size<Throwing_variable_buffer_message>(payload);
    const size_t         total_size =
        sizeof(Throwing_variable_buffer_message) + extra_bytes;

    void* raw = ::operator new(
        total_size,
        std::align_val_t(alignof(Throwing_variable_buffer_message)));

    bool ok = expect_throw<std::runtime_error>(
        "variable_buffer context is restored after body constructor throws",
        [&]()
        {
            new (raw) Throwing_variable_buffer_message(payload);
        });

    ::operator delete(
        raw,
        std::align_val_t(alignof(Throwing_variable_buffer_message)));

    return ok &&
        sintra::test::assert_true(
            sintra::variable_buffer::tl_message_start_address == nullptr &&
            sintra::variable_buffer::tl_pbytes_to_next_message == nullptr,
            k_prefix,
            "variable_buffer TLS context should be cleared after constructor failure"
        );
}

bool test_align_up_size_overflow()
{
    const size_t value = std::numeric_limits<size_t>::max() - 3;
    return expect_throw<std::overflow_error>(
        "align_up_size overflow",
        [&]() { (void)sintra::detail::align_up_size(value, 8); });
}

bool test_message_ring_rejects_misaligned_frame_length()
{
    const auto directory =
        sintra::test::unique_scratch_directory("fault_injection_misaligned_message_frame");
    struct Cleanup
    {
        std::filesystem::path directory;
        ~Cleanup()
        {
            std::error_code ec;
            (void)std::filesystem::remove_all(directory, ec);
        }
    } cleanup{directory};

    sintra::Message_ring_R reader(directory.string(), "message_ring", 0xFA17u);
    reader.start_reading();

    sintra::Message_prefix prefix;
    uint32_t bytes_to_next =
        static_cast<uint32_t>(sizeof(sintra::Message_prefix) + 1);
    if ((bytes_to_next % sintra::detail::message_frame_alignment) == 0) {
        ++bytes_to_next;
    }
    prefix.bytes_to_next_message = bytes_to_next;

    std::array<char, sizeof(sintra::Message_prefix) + 16> raw{};
    std::memcpy(raw.data(), &prefix, sizeof(prefix));

    sintra::Message_ring_W writer(directory.string(), "message_ring", 0xFA17u);
    writer.write(raw.data(), bytes_to_next);
    writer.done_writing();

    return expect_throw<sintra::corrupted_message_exception>(
        "message ring rejects misaligned frame length",
        [&]() { (void)reader.fetch_message(); });
}

bool test_message_frame_size_limit_is_enforced_before_write()
{
    std::vector<uint8_t> payload(sintra::detail::message_frame_size_limit, 0);
    return expect_throw<std::length_error>(
        "message frame size limit is enforced before write",
        [&]() { (void)sintra::vb_size<Large_variable_buffer_message>(payload); });
}

bool test_process_index_exhaustion()
{
    bool      threw    = false;
    const int attempts = sintra::max_process_index + 4;
    for (int i = 0; i < attempts; ++i) {
        try {
            (void)sintra::make_process_instance_id();
        }
        catch (const std::runtime_error&) {
            threw = true;
            break;
        }
    }

    return
        sintra::test::assert_true(
            threw,
            k_prefix,
            "make_process_instance_id should throw when process index space is exhausted"
        );
}

} // namespace

int main()
{
    bool ok = true;
    ok &= test_variable_buffer_container_too_large();
    ok &= test_variable_buffer_payload_too_large();
    ok &= test_variable_buffer_requires_message_context();
    ok &= test_variable_buffer_context_restored_after_throw();
    ok &= test_align_up_size_overflow();
    ok &= test_message_ring_rejects_misaligned_frame_length();
    ok &= test_message_frame_size_limit_is_enforced_before_write();
    ok &= test_process_index_exhaustion();
    return ok ? 0 : 1;
}
