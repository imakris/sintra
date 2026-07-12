// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../id_types.h"
#include "../ipc/rings.h"
#include "../utility.h"
#include "../messaging/message_args.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <format>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace sintra {

using std::enable_if_t;
using std::is_base_of;
using std::is_convertible;
using std::is_same;
using std::is_standard_layout_v;
using std::is_trivial_v;
using std::remove_cv;
using std::remove_reference;
using std::string;

constexpr uint64_t message_magic     = 0xc18a1aca1ebac17a;
constexpr int      message_ring_size = 0x200000;


 //////////////////////////////////////////////////////////////////////////
///// BEGIN VARIABLE BUFFER ////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


// This type only lives inside ring buffers.
struct variable_buffer
{
    // this is set in the constructor of the derived type
    size_t                                 num_bytes                 = 0;

    // this is set by the message constructor
    size_t                                 offset_in_bytes           = 0;

    inline static thread_local char*       tl_message_start_address  = nullptr;
    inline static thread_local uint32_t*   tl_pbytes_to_next_message = nullptr;

    bool empty() const { return num_bytes == 0; }

    variable_buffer() {}

    variable_buffer(const variable_buffer&) = default;
    variable_buffer& operator=(const variable_buffer&) = default;

    template <typename TC, typename T = typename TC::iterator::value_type>
    variable_buffer(const TC& container);
};



namespace detail
{

template <typename T, typename = void>
struct typed_variable_buffer_alignment
{
    static constexpr std::size_t value = alignof(variable_buffer);
};

template <typename T>
struct typed_variable_buffer_alignment<
    T,
    std::void_t<typename std::decay_t<T>::value_type>
>
{
    using value_type = typename std::decay_t<T>::value_type;
    static constexpr std::size_t value = std::max(alignof(value_type), alignof(variable_buffer));
};

} // namespace detail


template <typename T>
struct alignas(detail::typed_variable_buffer_alignment<T>::value) typed_variable_buffer: protected variable_buffer
{
public:
    typed_variable_buffer() {}
    typed_variable_buffer(const T& v): variable_buffer(v) {}
    template <typename CT = typename T::iterator::value_type>
    operator T() const
    {
        static_assert(
            std::is_trivially_copyable_v<CT>,
            "typed_variable_buffer requires trivially copyable value types."
        );
        assert(offset_in_bytes);
        assert((num_bytes % sizeof(CT)) == 0);

        const auto* typed_data = reinterpret_cast<const CT*>(
            reinterpret_cast<const char*>(this) + offset_in_bytes);
        assert(
            (reinterpret_cast<std::uintptr_t>(typed_data) % alignof(CT)) == 0 &&
            "typed_variable_buffer storage must be properly aligned."
        );
        size_t num_elements = num_bytes / sizeof(CT);
        return T(typed_data, typed_data + num_elements);
    }

    size_t size_bytes()        const { return num_bytes;       }
    size_t data_offset_bytes() const { return offset_in_bytes; }
    const void* data_address() const
    {
        return reinterpret_cast<const char*>(this) + offset_in_bytes;
    }
};


template <typename T>
struct is_variable_buffer_argument
    : std::bool_constant<
        is_convertible<T, variable_buffer>::value ||
        is_base_of<variable_buffer, std::remove_cvref_t<T>>::value
    >
{};


namespace detail
{

inline constexpr size_t message_frame_alignment =
    (alignof(std::max_align_t) > 16u) ? alignof(std::max_align_t) : 16u;
inline constexpr size_t message_frame_size_limit =
    static_cast<size_t>(message_ring_size) / 8u;

inline size_t align_up_size(size_t value, size_t alignment)
{
    if (alignment <= 1) {
        return value;
    }

    const size_t remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }

    const size_t padding = alignment - remainder;
    if (padding > (std::numeric_limits<size_t>::max() - value)) {
        throw std::overflow_error("variable_buffer alignment overflow");
    }

    return value + padding;
}

inline size_t checked_add_size(size_t lhs, size_t rhs, const char* message)
{
    if (rhs > (std::numeric_limits<size_t>::max() - lhs)) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

inline size_t finish_message_frame_size(size_t span_end)
{
    const size_t aligned = align_up_size(span_end, message_frame_alignment);
    if (aligned > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error("sintra message frame exceeds 32-bit length field");
    }
    if (aligned >= message_frame_size_limit) {
        throw std::length_error("sintra message frame exceeds maximum frame size");
    }
    return aligned;
}

template <typename T, typename = void>
struct has_value_type: std::false_type
{};

template <typename T>
struct has_value_type<T, std::void_t<typename std::decay_t<T>::value_type>>: std::true_type
{};

inline size_t accumulate_vb_size(size_t offset)
{
    return offset;
}

template <typename T, typename... Args>
size_t accumulate_vb_size(size_t offset, const T& value, Args&&... args)
{
    using decayed = std::remove_reference_t<T>;

    if constexpr (
        std::is_convertible_v<decayed, variable_buffer> &&
        has_value_type<decayed>::value)
    {
        using value_type = typename decayed::value_type;
        static_assert(
            std::is_trivially_copyable_v<value_type>,
            "variable_buffer payloads must be trivially copyable."
        );
        static_assert(
            alignof(value_type) <= message_frame_alignment,
            "variable_buffer payload alignment exceeds the message frame alignment."
        );

        if (value.size() >
            (std::numeric_limits<size_t>::max() / sizeof(value_type)))
        {
            throw std::overflow_error("variable_buffer payload size overflow");
        }

        const size_t data_bytes = value.size() * sizeof(value_type);
        offset = checked_add_size(
            align_up_size(offset, alignof(value_type)),
            data_bytes,
            "variable_buffer message span overflow");
        return accumulate_vb_size(offset, std::forward<Args>(args)...);
    }
    else {
        return accumulate_vb_size(offset, std::forward<Args>(args)...);
    }
}

class variable_buffer_context_guard
{
public:
    variable_buffer_context_guard(
        char*     message_start_address,
        uint32_t* pbytes_to_next_message) noexcept
    :
        m_old_message_start_address(variable_buffer::tl_message_start_address),
        m_old_pbytes_to_next_message(variable_buffer::tl_pbytes_to_next_message)
    {
        variable_buffer::tl_message_start_address  = message_start_address;
        variable_buffer::tl_pbytes_to_next_message = pbytes_to_next_message;
    }

    ~variable_buffer_context_guard() noexcept
    {
        variable_buffer::tl_message_start_address  = m_old_message_start_address;
        variable_buffer::tl_pbytes_to_next_message = m_old_pbytes_to_next_message;
    }

    variable_buffer_context_guard(const variable_buffer_context_guard&) = delete;
    variable_buffer_context_guard& operator=(const variable_buffer_context_guard&) = delete;

private:
    char*     m_old_message_start_address;
    uint32_t* m_old_pbytes_to_next_message;
};

inline variable_buffer_context_guard make_variable_buffer_context(
    char*     message_start_address,
    uint32_t* pbytes_to_next_message) noexcept
{
    return variable_buffer_context_guard(message_start_address, pbytes_to_next_message);
}

} // namespace detail


template <typename MESSAGE_T, typename... Args>
size_t vb_size(Args&&... args)
{
    const size_t base_size = sizeof(MESSAGE_T);
    const size_t final_offset = detail::accumulate_vb_size(
        base_size,
        std::forward<Args>(args)...);
    const size_t aligned_final_offset =
        detail::finish_message_frame_size(final_offset);

    assert(aligned_final_offset >= base_size);
    return aligned_final_offset - base_size;
}


struct void_placeholder_t
{};

using message_string = typed_variable_buffer<string>;


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END VARIABLE BUFFER //////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN SERIALIZABLE TYPE //////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


class Sintra_message_element {};


template <typename...>
struct always_false: std::false_type
{};


template <
    typename T,
    bool =
        (is_trivial_v<T> && is_standard_layout_v<T>) ||
        is_base_of<Sintra_message_element, T>::value,
    bool = is_variable_buffer_argument<T>::value
>
struct transformer
{
    static_assert(
        always_false<T>::value,
        "Unsupported message argument type. Provide a trivial standard-layout "
        "type, Sintra_message_element, or variable_buffer-compatible type."
    );
    using type = void;
};


template <typename T, bool IRRELEVANT>
struct transformer<T, true, IRRELEVANT>
{
    using type = T;
};


template <typename T>
struct transformer<T, false, true>
{
    using bare_type = std::remove_cvref_t<T>;
    using type      = std::conditional_t<
        is_base_of<variable_buffer, bare_type>::value,
        std::remove_reference_t<T>,
        typed_variable_buffer<bare_type>
    >;
};


template <typename SEQ_T, int I, int J, typename... Args>
struct serializable_type_impl;


template <typename SEQ_T, int I, typename... Args>
struct serializable_type_impl<SEQ_T, I, I, Args...>
{
    using type = detail::message_args<Args...>;
    static constexpr bool has_variable_buffers = false;
};


template <typename SEQ_T, int I, int J, typename... Args>
struct serializable_type_impl
{
    using arg_type         = typename detail::message_args_nth_type<SEQ_T, I>::type;
    using transformed_type =
        typename transformer<std::remove_reference_t<arg_type>>::type;
    using aggregate_type   =
        serializable_type_impl<SEQ_T, I + 1, J, Args..., transformed_type>;
    using type             = typename aggregate_type::type;
    static constexpr bool has_variable_buffers = aggregate_type::has_variable_buffers ||
        !is_same<
            std::remove_reference_t<arg_type>,
            std::remove_reference_t<transformed_type>
        >::value;
};


template <
    typename SEQ_T,
    typename AGGREGATE = serializable_type_impl<
        SEQ_T,
        0,
        detail::message_args_size<SEQ_T>::value
    >,
    typename BASE = typename AGGREGATE::type
>
struct serializable_type: BASE
{
    using BASE::BASE;
    static constexpr bool has_variable_buffers = AGGREGATE::has_variable_buffers;
    using message_args_type = typename BASE::message_args_type;
};


template<typename... Args>
constexpr bool args_require_varbuffer =
    serializable_type<detail::message_args<Args...>>::has_variable_buffers;


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END SERIALIZABLE TYPE ////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN MESSAGE ////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


// The prefix is the first part of any message sent through a ring.
// It contains information necessary for the dispatcher to deliver messages, which may as well
// be used by the recipient.
struct Message_prefix
{
    // used by the serializer as a basic corruption check
    const uint64_t     magic = message_magic;

    // used by the serializer, set in constructor
    uint32_t           bytes_to_next_message = 0;

    union
    {
        // This is a cross-process type id. It is set in message constructor.
        type_id_type message_type_id        = invalid_type_id;

        // Only relevant in RPC return messages.
        type_id_type exception_type_id;
    };

    // Only instantiated in RPC messages, to associate the return messages with the original.
    // When this is set, the message_type_id becomes irrelevant.
    instance_id_type   function_instance_id = invalid_instance_id;

    // used by the serializer, set in communicators
    instance_id_type   sender_instance_id   = invalid_instance_id;

    // used by the serializer, set in communicators
    instance_id_type   receiver_instance_id = invalid_instance_id;

    // Coordinator-stamped internal metadata for managed-child service RPCs.
    // These fields are zero for ordinary messages and are never caller authority.
    uint64_t           managed_child_custody_identity = 0;
    uint32_t           managed_child_occurrence       = 0;
};

struct corrupted_message_exception : public std::runtime_error
{
    explicit corrupted_message_exception(const char* what_arg)
        : std::runtime_error(what_arg)
    {}
};

template <typename T>
sintra::type_id_type get_type_id();

// This type only lives inside ring buffers.

template <
    typename T,
    typename RT = void,
    type_id_type ID = 0,
    typename EXPORTER = void
>
struct Message: public Message_prefix, public T
{
    using body_type   = T;
    using return_type = RT;
    using exporter    = EXPORTER;

    static_assert(
        alignof(T) <= detail::message_frame_alignment,
        "Sintra message body alignment exceeds the message frame alignment.");

    static constexpr type_id_type sintra_type_id()
    {
        if constexpr (ID != 0) {
            return ID;
        }
        return invalid_type_id;
    }

    Message(const Message&) = default;
    Message(Message&&) = default;
    Message& operator=(const Message&) = default;
    Message& operator=(Message&&) = default;


    static type_id_type id()
    {
        if constexpr (ID != 0) {
            static const type_id_type tid = make_type_id(ID);
            return tid;
        }
        else {
            return get_type_id<Message>();
        }
    }


    template<
        typename... Args, typename = void,
        typename = enable_if_t< !args_require_varbuffer<Args...> >
    >
    Message(Args&& ...args)
    :
        Message_prefix{
            .bytes_to_next_message = static_cast<uint32_t>(
                detail::finish_message_frame_size(sizeof(Message)))},
        T{std::forward<Args>(args)...}
    {
        assert(bytes_to_next_message < detail::message_frame_size_limit);

        message_type_id = id();
    }


    template<
        typename... Args,
        typename = enable_if_t< args_require_varbuffer<Args...> >
    >
    Message(Args&& ...args)
    :
        Message_prefix{
            .bytes_to_next_message = static_cast<uint32_t>(sizeof(Message))},
        // The discarded guard temporaries live through this full initializer,
        // keeping TLS active while the T base subobject is built.
        T{
            (detail::make_variable_buffer_context(
                reinterpret_cast<char*>(this),
                &bytes_to_next_message),
             std::forward<Args>(args))...
        }
    {
        bytes_to_next_message = static_cast<uint32_t>(
            detail::finish_message_frame_size(bytes_to_next_message));

        assert(bytes_to_next_message < detail::message_frame_size_limit);

        message_type_id = id();
    }


    ~Message()
    {
    }
};

// Mark sintra::Message payloads as explicitly allowed non-trivial ring payloads.
// They are constructed in-place inside the ring, but their layout and lifetime
// are controlled by the messaging layer and do not own heap memory beyond the
// ring mapping itself.
template <typename T, typename RT, type_id_type ID, typename EXPORTER>
struct ring_payload_traits<Message<T, RT, ID, EXPORTER>>
{
    static constexpr bool allow_nontrivial = true;
};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END MESSAGE //////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN MESSAGE MACROS /////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


// Variadic macro overloading helper: dispatches SINTRA_MESSAGE_BASE to a
// VA_DEFINE_STRUCT_N variant based on argument count. The double-glue
// indirection is the standard workaround for MSVC preprocessors that treat
// __VA_ARGS__ as a single token, and stays portable across compilers without
// requiring /Zc:preprocessor on MSVC.
//
// Cap is 16 message fields; widen the VA_DEFINE_STRUCT_N family below if more
// are ever needed.

/* internal helpers */
#define VA_NARGS_GLUE_(x, y) x y
#define VA_NARGS_RETURN_COUNT_(                                         \
    _1_ , _2_ , _3_ , _4_ , _5_ , _6_ , _7_ , _8_,                      \
    _9_ ,_10_, _11_, _12_, _13_, _14_, _15_, _16_, count, ...) count
#define VA_NARGS_EXPAND_(args) VA_NARGS_RETURN_COUNT_ args
#define VA_NARGS_COUNT_MAX16_(...) VA_NARGS_EXPAND_((__VA_ARGS__,       \
    16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))

#define VA_NARGS_OVERLOAD_MACRO2_(name, count) name##count
#define VA_NARGS_OVERLOAD_MACRO1_(name, count) VA_NARGS_OVERLOAD_MACRO2_(name, count)
#define VA_NARGS_OVERLOAD_MACRO_(name,  count) VA_NARGS_OVERLOAD_MACRO1_(name, count)

/* expose for re-use */
#define VA_NARGS_CALL_OVERLOAD(name, ...) \
    VA_NARGS_GLUE_(VA_NARGS_OVERLOAD_MACRO_(name, VA_NARGS_COUNT_MAX16_(__VA_ARGS__)),(__VA_ARGS__))

/* overloads */
#define VA_DEFINE_STRUCT_1( v)                                  struct v { };
#define VA_DEFINE_STRUCT_2( v,a)                                struct v { a; };
#define VA_DEFINE_STRUCT_3( v,a,b)                              struct v { a;b; };
#define VA_DEFINE_STRUCT_4( v,a,b,c)                            struct v { a;b;c; };
#define VA_DEFINE_STRUCT_5( v,a,b,c,d)                          struct v { a;b;c;d; };
#define VA_DEFINE_STRUCT_6( v,a,b,c,d,e)                        struct v { a;b;c;d;e; };
#define VA_DEFINE_STRUCT_7( v,a,b,c,d,e,f)                      struct v { a;b;c;d;e;f; };
#define VA_DEFINE_STRUCT_8( v,a,b,c,d,e,f,g)                    struct v { a;b;c;d;e;f;g; };
#define VA_DEFINE_STRUCT_9( v,a,b,c,d,e,f,g,h)                  struct v { a;b;c;d;e;f;g;h; };
#define VA_DEFINE_STRUCT_10(v,a,b,c,d,e,f,g,h,i)                struct v { a;b;c;d;e;f;g;h;i; };
#define VA_DEFINE_STRUCT_11(v,a,b,c,d,e,f,g,h,i,j)              struct v { a;b;c;d;e;f;g;h;i;j; };
#define VA_DEFINE_STRUCT_12(v,a,b,c,d,e,f,g,h,i,j,k)            struct v { a;b;c;d;e;f;g;h;i;j;k; };
#define VA_DEFINE_STRUCT_13(v,a,b,c,d,e,f,g,h,i,j,k,l)          struct v { a;b;c;d;e;f;g;h;i;j;k;l; };
#define VA_DEFINE_STRUCT_14(v,a,b,c,d,e,f,g,h,i,j,k,l,m)        struct v { a;b;c;d;e;f;g;h;i;j;k;l;m; };
#define VA_DEFINE_STRUCT_15(v,a,b,c,d,e,f,g,h,i,j,k,l,m,n)      struct v { a;b;c;d;e;f;g;h;i;j;k;l;m;n; };
#define VA_DEFINE_STRUCT_16(v,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o)    struct v { a;b;c;d;e;f;g;h;i;j;k;l;m;n;o; };

/* reusable macro */
#define DEFINE_STRUCT_(...) VA_NARGS_CALL_OVERLOAD(VA_DEFINE_STRUCT_, __VA_ARGS__)

#define SINTRA_TYPE_ID(idv)                                                             \
    static_assert((idv) > 0, "SINTRA_TYPE_ID id must be non-zero.");                    \
    static_assert(                                                                      \
        (idv) <= sintra::max_user_type_id,                                              \
        "SINTRA_TYPE_ID id must fit in the user id range."                              \
    );                                                                                  \
    static constexpr sintra::type_id_type sintra_type_id()                              \
    {                                                                                   \
        return sintra::make_user_type_id(idv);                                          \
    }


#define SINTRA_MESSAGE_BASE(name, idv, ...)                                             \
    void inheritance_assertion_##name() {                                               \
        static_assert(std::is_same_v<                                                   \
            std::remove_pointer_t<decltype(this)>,                                      \
            Transceiver_type>,                                                          \
            "This Transceiver is not derived correctly."                                \
        );                                                                              \
    }                                                                                   \
    DEFINE_STRUCT_(sm_body_type_##name, __VA_ARGS__)                                    \
    using name = sintra::Message<sm_body_type_##name, void, idv, Transceiver_type>;     \


#define SINTRA_MESSAGE(name, ...)                                                       \
    SINTRA_MESSAGE_BASE(name, sintra::invalid_type_id, __VA_ARGS__)

#define SINTRA_MESSAGE_EXPLICIT(name, idv, ...)                                         \
    static_assert((idv) > 0, "SINTRA_MESSAGE_EXPLICIT id must be non-zero.");           \
    static_assert(                                                                      \
        (idv) <= sintra::max_user_type_id,                                              \
        "SINTRA_MESSAGE_EXPLICIT id must fit in the user id range."                     \
    );                                                                                  \
    SINTRA_MESSAGE_BASE(name, sintra::make_user_type_id(idv), __VA_ARGS__)

#define SINTRA_MESSAGE_RESERVED(name, ...)                                              \
    SINTRA_MESSAGE_BASE(name, (type_id_type)sintra::detail::reserved_id::name, __VA_ARGS__)



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END MESSAGE MACROS ///////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN ENCLOSURE //////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


template <
    typename T,
    bool = is_convertible<T, variable_buffer>::value,
    bool = is_trivial_v<T> && is_standard_layout_v<T>
>
struct Enclosure
{
};


template<>
struct Enclosure<void, false, false>
{
    void get_value() const {}
};


template <typename T>
struct Enclosure<T, false, true>
{
    T get_value() const { return value; }
    T value;
};


template <typename T>
struct Enclosure<T, true, false>
{
    T get_value() const { return value; }
    typed_variable_buffer<std::remove_reference_t<T>> value;
};



template <
    typename T,
    bool C1 = is_convertible<T, variable_buffer>::value,
    bool C2 = is_trivial_v<T> && is_standard_layout_v<T>
>
struct Unserialized_Enclosure: Enclosure <T, C1, C2>
{
    Unserialized_Enclosure& operator=(const Enclosure<T, C1, C2>& rhs)
    {
        Enclosure<T, C1, C2>::operator=(rhs);
        return *this;
    }
};



template <typename T>
struct Unserialized_Enclosure<T, true, false>: Enclosure <T, true, false>
{
    Unserialized_Enclosure() {}

    Unserialized_Enclosure& operator=(const Enclosure<T, true, false>& rhs)
    {
        free_value = rhs.value;
        return *this;
    }

    std::remove_reference_t<T> free_value;

    T get_value() const { return free_value; }
};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END ENCLOSURE ////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN MESSAGE RINGS //////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


inline
std::string get_base_filename(const string& prefix, uint64_t id, uint32_t occurrence = 0)
{
    if (occurrence > 0) {
        return std::format("{}{:x}_occ{}", prefix, id, occurrence);
    }
    return std::format("{}{:x}", prefix, id);
}


struct Message_ring_R: Ring_R<char>
{
    Message_ring_R(const string& directory, const string& prefix, uint64_t id, uint32_t occurrence = 0):
        Ring_R(directory, get_base_filename(prefix, id, occurrence), message_ring_size),
        m_id(id)
    {}


    void done_reading()
    {
        Ring_R::done_reading();
        m_range = decltype(m_range)();
    }

    // Returns a pointer to the buffer of the message
    // If there is no message to read, it blocks.
    Message_prefix* fetch_message()
    {
        // if all the messages in the reading buffer have been read
        if (m_range.begin == m_range.end) {
            // if this is not an uninitialized state
            if (m_reading) {
                // finalize the reading
                done_reading_new_data();
            }
            else {
                // initialize for all subsequent reads
                start_reading();
            }

            // start with a new reading buffer. this will block until there is something to read.
            while (true) {
                auto range = wait_for_new_data();
                if (!range.begin) {
                    if (consume_eviction_notification()) {
                        continue;
                    }
                    if (is_stopping()) {
                        return nullptr;
                    }
                    continue;
                }
                m_range = range;
                break;
            }
        }

        bool f = false;
        detail::Spin_backoff backoff;
        while (!m_reading_lock.compare_exchange_strong(f, true)) {
            f = false;
            backoff.spin();
        }

        //m_reading_lock
        if (!m_reading) {
            m_reading_lock = false;
            return nullptr;
        }

        auto fail = [this](const char* reason) -> Message_prefix*
        {
            m_reading_lock = false;
            throw corrupted_message_exception(reason);
        };

        if (m_range.end < m_range.begin) {
            return fail("Sintra message ring contains an invalid readable range.");
        }

        const auto available_bytes = static_cast<size_t>(m_range.end - m_range.begin);
        if (available_bytes < sizeof(Message_prefix)) {
            return fail("Sintra message ring contains a truncated message prefix.");
        }

        Message_prefix* ret = reinterpret_cast<Message_prefix*>(m_range.begin);
        if (ret->magic != message_magic) {
            return fail("Sintra message ring contains an invalid message magic.");
        }

        const uint32_t bytes_to_next = ret->bytes_to_next_message;
        if (bytes_to_next < sizeof(Message_prefix)) {
            return fail("Sintra message ring contains an invalid message length.");
        }

        if ((bytes_to_next % detail::message_frame_alignment) != 0) {
            return fail("Sintra message ring contains a misaligned message frame length.");
        }

        if (bytes_to_next >= detail::message_frame_size_limit) {
            return fail("Sintra message ring contains a message larger than the maximum frame size.");
        }

        if (bytes_to_next > available_bytes) {
            return fail("Sintra message ring contains a message that exceeds the readable range.");
        }

        m_range.begin += bytes_to_next;

        m_reading_lock = false;
        return ret;
    }

    sequence_counter_type get_message_reading_sequence() const
    {
        return reading_sequence() - (m_range.end - m_range.begin);
    }

public:
    const uint64_t m_id;

protected:
    Range<char>    m_range;
};



struct Message_ring_W: public Ring_W<char>
{
    Message_ring_W(const string& directory, const string& prefix, uint64_t id, uint32_t occurrence = 0) :
        Ring_W(directory, get_base_filename(prefix, id, occurrence), message_ring_size),
        m_id(id)
    {}

    Message_ring_W(const Message_ring_W&) = delete;
    const Message_ring_W& operator = (const Message_ring_W&) = delete;

    void relay(
        const Message_prefix& msg,
        uint64_t managed_child_custody_identity = 0,
        uint32_t managed_child_occurrence = 0)
    {
        auto* relayed = reinterpret_cast<Message_prefix*>(
            write((const char*)&msg, msg.bytes_to_next_message));
        relayed->managed_child_custody_identity = managed_child_custody_identity;
        relayed->managed_child_occurrence = managed_child_occurrence;
        done_writing();
    }

public:
    const uint64_t m_id;
};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END MESSAGE RINGS ////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////

} // namespace sintra
