#include <sintra/detail/messaging/message_args.h>
#include <sintra/detail/messaging/call_function_with_message_args.h>

#include <type_traits>

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

    struct Zero_arg_target {
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

    return 0;
}
