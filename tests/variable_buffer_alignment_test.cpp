#include <sintra/detail/messaging/message.h>
#include <sintra/detail/messaging/message_impl.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <new>
#include <type_traits>
#include <vector>

namespace {

struct alignas(16) Aligned16
{
    std::array<std::uint8_t, 16> data{};
};

static_assert(std::is_trivially_copyable_v<Aligned16>, "Aligned16 must be trivially copyable");

struct Alignment_payload
{
    sintra::typed_variable_buffer<std::vector<char>>       chars;
    sintra::typed_variable_buffer<std::vector<Aligned16>>  aligned_values;
    sintra::typed_variable_buffer<std::vector<double>>     doubles;
};

struct One_byte_payload
{
    sintra::typed_variable_buffer<std::vector<char>> chars;
};

struct Plain_payload
{
    std::uint64_t value = 0x123456789abcdef0ull;
};

bool is_aligned(const void* ptr, std::size_t alignment)
{
    return (reinterpret_cast<std::uintptr_t>(ptr) % alignment) == 0;
}

} // namespace

using Alignment_message = sintra::Message<Alignment_payload, void, 0xA11E71Dull>;
using One_byte_message  = sintra::Message<One_byte_payload, void, 0xA11E71Eull>;
using Plain_message     = sintra::Message<Plain_payload, void, 0xA11E71Full>;

int main()
{
    std::vector<char> chars = {'a', 'l', 'i', 'g', 'n', 'm', 'e', 'n', 't'};
    std::vector<Aligned16> aligned_values(3);
    std::vector<double> doubles = {1.0, 2.0, 3.0, 4.0};

    const std::size_t expected_extra = sintra::vb_size<Alignment_message>(chars, aligned_values, doubles);
    const std::size_t total_size     = sizeof(Alignment_message) + expected_extra;

    void*              raw     = ::operator new(total_size, std::align_val_t(alignof(Alignment_message)));
    Alignment_message* message = new (raw) Alignment_message(chars, aligned_values, doubles);

    const auto* base        = reinterpret_cast<const char*>(message);
    const auto* chars_ptr   = static_cast<const char*>(message->chars.data_address());
    const auto* aligned_ptr = static_cast<const Aligned16*>(message->aligned_values.data_address());
    const auto* doubles_ptr = static_cast<const double*>(message->doubles.data_address());

    auto absolute_offset = [&](const void* ptr) {
        return static_cast<std::size_t>(reinterpret_cast<const char*>(ptr) - base);
    };

    auto cleanup_and_fail = [&](int code) {
        message->~Alignment_message();
        ::operator delete(raw, std::align_val_t(alignof(Alignment_message)));
        return code;
    };

    if (!is_aligned(aligned_ptr, alignof(Aligned16))) {
        std::cerr << "Aligned payload pointer is not correctly aligned" << std::endl;
        return cleanup_and_fail(1);
    }

    if (!is_aligned(doubles_ptr, alignof(double))) {
        std::cerr << "Double payload pointer is not correctly aligned" << std::endl;
        return cleanup_and_fail(1);
    }

    if (message->aligned_values.size_bytes() != aligned_values.size() * sizeof(Aligned16)) {
        std::cerr << "Aligned payload size accounting mismatch" << std::endl;
        return cleanup_and_fail(1);
    }

    if (message->doubles.size_bytes() != doubles.size() * sizeof(double)) {
        std::cerr << "Double payload size accounting mismatch" << std::endl;
        return cleanup_and_fail(1);
    }

    const std::size_t last_payload_end = absolute_offset(doubles_ptr) + message->doubles.size_bytes();
    if (message->bytes_to_next_message < last_payload_end) {
        std::cerr << "Message byte span ends before the last payload" << std::endl;
        return cleanup_and_fail(1);
    }

    if ((message->bytes_to_next_message - last_payload_end) >=
        sintra::detail::message_frame_alignment)
    {
        std::cerr << "Message frame padding is larger than expected" << std::endl;
        return cleanup_and_fail(1);
    }

    if (!is_aligned(base + message->bytes_to_next_message,
            sintra::detail::message_frame_alignment))
    {
        std::cerr << "Message frame end is not correctly aligned" << std::endl;
        return cleanup_and_fail(1);
    }

    if (message->bytes_to_next_message - sizeof(Alignment_message) != expected_extra) {
        std::cerr << "vb_size did not predict the aligned payload span" << std::endl;
        return cleanup_and_fail(1);
    }

    if (!is_aligned(chars_ptr, alignof(char))) {
        std::cerr << "Char payload pointer should always be aligned" << std::endl;
        return cleanup_and_fail(1);
    }

    message->~Alignment_message();
    ::operator delete(raw, std::align_val_t(alignof(Alignment_message)));

    std::vector<char> single_char = {'x'};
    const std::size_t first_frame =
        sizeof(One_byte_message) + sintra::vb_size<One_byte_message>(single_char);
    const std::size_t second_frame =
        sizeof(Plain_message) + sintra::vb_size<Plain_message>();
    const std::size_t combined_size = first_frame + second_frame;

    void* combined_raw = ::operator new(
        combined_size,
        std::align_val_t(sintra::detail::message_frame_alignment));
    auto* first = new (combined_raw) One_byte_message(single_char);
    auto* second_address = reinterpret_cast<char*>(combined_raw) +
        first->bytes_to_next_message;

    if (first->bytes_to_next_message != first_frame) {
        std::cerr << "One-byte variable frame size did not match vb_size" << std::endl;
        first->~One_byte_message();
        ::operator delete(
            combined_raw,
            std::align_val_t(sintra::detail::message_frame_alignment));
        return 1;
    }

    if (!is_aligned(second_address, alignof(Plain_message))) {
        std::cerr << "Second message address is not aligned after one-byte variable payload" << std::endl;
        first->~One_byte_message();
        ::operator delete(
            combined_raw,
            std::align_val_t(sintra::detail::message_frame_alignment));
        return 1;
    }

    auto* second = new (second_address) Plain_message();
    second->~Plain_message();
    first->~One_byte_message();
    ::operator delete(
        combined_raw,
        std::align_val_t(sintra::detail::message_frame_alignment));
    return 0;
}
