#include <iostream>
#include <type_traits>

#include <sintra/detail/message_args.h>

namespace {

using args_t = sintra::detail::message_args<int, double>;

static_assert(
    sintra::detail::message_args_size<args_t&>::value == 2,
    "message_args_size should ignore references");
static_assert(
    sintra::detail::message_args_size<const args_t&>::value == 2,
    "message_args_size should ignore cv-qualified references");
static_assert(
    std::is_same<
        double,
        sintra::detail::message_args_nth_type<args_t&, 1>::type>::value,
    "message_args_nth_type should ignore references");

} // namespace

int main() {
    std::cout << "Dummy test executed successfully." << std::endl;
    return 0;
}