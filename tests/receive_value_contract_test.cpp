#include <sintra/sintra.h>

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct Fixed_fields { int value; double other; };
struct Trivial_constructor { Trivial_constructor() = default; int value; };
struct Simple_constructor { explicit Simple_constructor(int v) : value(v) {} int value; };
struct Nested_descriptor { sintra::message_string text; };
using Text = sintra::message_string;
using Numbers = sintra::typed_variable_buffer<std::vector<int>>;

struct Receive_types : sintra::Derived_transceiver<Receive_types>
{
    SINTRA_MESSAGE(Empty);
    SINTRA_MESSAGE(Fixed, unsigned sequence);
    SINTRA_MESSAGE(Initialized, unsigned sequence = 0);
    SINTRA_MESSAGE(Aggregate, Fixed_fields fields, int values[3]);
    SINTRA_MESSAGE(Trivial_construction, Trivial_constructor value);
    SINTRA_MESSAGE(Constructed_value, Simple_constructor value);
    SINTRA_MESSAGE(Mixed, Fixed_fields fixed, Text text, Numbers numbers);
    SINTRA_MESSAGE(Identity, sintra::Resolvable_instance_id value);
    SINTRA_MESSAGE(Text_message, Text text);
    SINTRA_MESSAGE(Vector_message, Numbers numbers);
    SINTRA_MESSAGE(Nested, Nested_descriptor nested);
    SINTRA_MESSAGE(Array, Text text[2]);
    SINTRA_MESSAGE_EXPLICIT(Explicit, 0x6543, Text text);
};

static_assert(sizeof(Receive_types::Fixed::body_type) == sizeof(unsigned));
static_assert(std::is_aggregate_v<Receive_types::Initialized::body_type>);
static_assert(std::is_trivially_copyable_v<Receive_types::Initialized::body_type>);
template <typename T>
concept Value_receivable = requires(sintra::Typed_instance_id<Receive_types> sender) {
    sintra::receive<T>();
    sintra::receive<T>(sender);
};

template <typename T>
concept Neither_value_receive_available =
    !requires { sintra::receive<T>(); } &&
    !requires(sintra::Typed_instance_id<Receive_types> sender) { sintra::receive<T>(sender); };

static_assert(Value_receivable<int>);
static_assert(Value_receivable<std::string>);
static_assert(Value_receivable<std::vector<int>>);
static_assert(Value_receivable<Receive_types::Empty>);
static_assert(Value_receivable<Receive_types::Fixed>);
static_assert(Value_receivable<Receive_types::Initialized>);
static_assert(Value_receivable<Receive_types::Aggregate>);
static_assert(Value_receivable<Receive_types::Identity>);
static_assert(Value_receivable<Receive_types::Trivial_construction>);
// User-provided constructors make a plain field nontrivial, outside the
// documented fixed-field value contract. Full-frame ownership still supports
// this trivially copyable representation.
static_assert(!std::is_trivial_v<Simple_constructor>);
static_assert(std::is_trivially_copyable_v<Simple_constructor>);
static_assert(Neither_value_receive_available<Receive_types::Constructed_value>);
static_assert(Neither_value_receive_available<Receive_types::Mixed>);
static_assert(Neither_value_receive_available<Receive_types::Text_message>);
static_assert(Neither_value_receive_available<Receive_types::Vector_message>);
static_assert(Neither_value_receive_available<Receive_types::Nested>);
static_assert(Neither_value_receive_available<Receive_types::Array>);
static_assert(Neither_value_receive_available<Receive_types::Explicit>);

template <typename T>
void accepted_value_calls(sintra::Typed_instance_id<Receive_types> sender)
{
    (void)sintra::receive<T>();
    (void)sintra::receive<T>(sender);
}

template <typename T>
void accepted_owned_calls(sintra::Typed_instance_id<Receive_types> sender)
{
    (void)sintra::receive_owned<T>();
    (void)sintra::receive_owned<T>(sender);
}

// This function is compiled, but never called: it checks the actual receive
// templates without starting a runtime or waiting for messages.
[[maybe_unused]] void accepted_calls(sintra::Typed_instance_id<Receive_types> sender)
{
    Receive_types::Fixed original(73u);
    const Receive_types::Fixed& source = original;
    [[maybe_unused]] Receive_types::Fixed const_copy(source);
    [[maybe_unused]] Receive_types::Fixed mutable_copy(original);
    [[maybe_unused]] Receive_types::Fixed moved(std::move(original));

    accepted_value_calls<int>(sender);
    accepted_value_calls<std::string>(sender);
    accepted_value_calls<std::vector<int>>(sender);
    accepted_value_calls<Receive_types::Empty>(sender);
    accepted_value_calls<Receive_types::Fixed>(sender);
    accepted_value_calls<Receive_types::Initialized>(sender);
    accepted_value_calls<Receive_types::Aggregate>(sender);
    accepted_value_calls<Receive_types::Identity>(sender);
    accepted_value_calls<Receive_types::Trivial_construction>(sender);
    accepted_owned_calls<Receive_types::Text_message>(sender);
    accepted_owned_calls<Receive_types::Vector_message>(sender);
    accepted_owned_calls<Receive_types::Nested>(sender);
    accepted_owned_calls<Receive_types::Array>(sender);
    accepted_owned_calls<Receive_types::Explicit>(sender);
    accepted_owned_calls<Receive_types::Mixed>(sender);
    accepted_owned_calls<Receive_types::Constructed_value>(sender);
}

// Compiler-negative probes use this same fixture with case 1..6. Each must
// fail with the receive_owned diagnostic, not merely reject a trait assertion.
#if defined(SINTRA_RECEIVE_REJECT_CASE)
[[maybe_unused]] void rejected_call(sintra::Typed_instance_id<Receive_types> sender)
{
#if SINTRA_RECEIVE_REJECT_CASE == 1
    (void)sintra::receive<Receive_types::Text_message>();
#elif SINTRA_RECEIVE_REJECT_CASE == 2
    (void)sintra::receive<Receive_types::Vector_message>(sender);
#elif SINTRA_RECEIVE_REJECT_CASE == 3
    (void)sintra::receive<Receive_types::Nested>();
#elif SINTRA_RECEIVE_REJECT_CASE == 4
    (void)sintra::receive<Receive_types::Array>(sender);
#elif SINTRA_RECEIVE_REJECT_CASE == 5
    (void)sintra::receive<Receive_types::Explicit>();
#elif SINTRA_RECEIVE_REJECT_CASE == 6
    (void)sintra::receive<Receive_types::Text_message>(sender);
#else
#error Unknown SINTRA_RECEIVE_REJECT_CASE
#endif
}
#endif

} // namespace

int main() {}
