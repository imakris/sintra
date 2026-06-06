#include <sintra/detail/messaging/message_args.h>
#include <sintra/detail/messaging/call_function_with_message_args.h>
#include <sintra/detail/messaging/message.h>

#include <type_traits>
#include <utility>

namespace {

struct Move_counted_payload : sintra::Sintra_message_element
{
    inline static int copies = 0;
    inline static int moves  = 0;

    int value = 0;

    Move_counted_payload() = default;
    explicit Move_counted_payload(int v) : value(v) {}

    Move_counted_payload(const Move_counted_payload& other)
        : value(other.value)
    {
        ++copies;
    }

    Move_counted_payload(Move_counted_payload&& other) noexcept
        : value(other.value)
    {
        other.value = -1;
        ++moves;
    }

    Move_counted_payload& operator=(const Move_counted_payload&) = default;
    Move_counted_payload& operator=(Move_counted_payload&&) = default;
};

struct Move_counted_body
{
    Move_counted_payload payload;
};

void reset_move_counts()
{
    Move_counted_payload::copies = 0;
    Move_counted_payload::moves  = 0;
}

} // namespace

int main()
{
    using sintra::detail::message_args;
    using sintra::detail::message_args_nth_type;
    using sintra::detail::message_args_size;

    using args_t = message_args<int, const double&>;

    static_assert(message_args_size<args_t>::value == 2, "Unexpected message_args_size result");
    static_assert(message_args_size<const args_t&>::value == 2,
        "message_args_size should ignore cvref qualifiers");

    static_assert(std::is_same<message_args_nth_type<args_t, 0>::type, int>::value,
        "Unexpected first argument type");
    static_assert(std::is_same<message_args_nth_type<const args_t&, 1>::type, const double&>::value,
        "message_args_nth_type should ignore cvref qualifiers");

    struct Zero_arg_target
    {
        int called = 0;
        int ping()
        {
            ++called;
            return 7;
        }
    };

    Zero_arg_target target;
    sintra::detail::message_args<> empty_args;
    const int result = sintra::call_function_with_message_args(target, &Zero_arg_target::ping, empty_args);
    if (result != 7 || target.called != 1) {
        return 1;
    }

    using counted_message = sintra::Message<Move_counted_body, void, 1>;

    Move_counted_payload lvalue_payload(11);
    reset_move_counts();
    counted_message copied(lvalue_payload);
    if (copied.payload.value != 11 ||
        Move_counted_payload::copies != 1 ||
        Move_counted_payload::moves != 0)
    {
        return 2;
    }

    Move_counted_payload rvalue_payload(17);
    reset_move_counts();
    counted_message moved(std::move(rvalue_payload));
    if (moved.payload.value != 17 ||
        rvalue_payload.value != -1 ||
        Move_counted_payload::copies != 0 ||
        Move_counted_payload::moves != 1)
    {
        return 3;
    }

    return 0;
}
