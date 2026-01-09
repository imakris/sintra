// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../id_types.h"
#include "../ipc/rings.h"
#include "../utility.h"
#include "../messaging/message_args.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace sintra {

namespace detail {

inline void spin_pause() noexcept
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__arm__)
    __asm__ __volatile__("yield");
#else
    #if defined(_MSC_VER)
        #pragma message("Sintra: unsupported architecture; spin_pause is a no-op and performance may degrade.")
    #elif defined(__GNUC__) || defined(__clang__)
        #warning "Sintra: unsupported architecture; spin_pause is a no-op and performance may degrade."
    #endif
    // No-op fallback for other architectures
#endif
}

} // namespace detail


using std::enable_if_t;
using std::is_base_of;
using std::is_convertible;
using std::is_same;
using std::is_standard_layout_v;
using std::is_trivial_v;
using std::remove_cv;
using std::remove_reference;


constexpr uint64_t  message_magic        = 0xc18a1aca1ebac17a;
constexpr int       message_ring_size    = 0x200000;


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
    size_t num_bytes = 0;

    // this is set by the message constructor
    size_t offset_in_bytes = 0;

    template <typename = void>
    struct Statics
    {
        thread_local static char*        tl_message_start_address;
        thread_local static uint32_t*    tl_pbytes_to_next_message;
    };
    using S = Statics<void>;

    bool empty() const { return num_bytes == 0; }

    variable_buffer() {}

    variable_buffer(const variable_buffer&) = default;
    variable_buffer& operator=(const variable_buffer&) = default;

    template <typename TC, typename T = typename TC::iterator::value_type>
    variable_buffer(const TC& container);
};


template <> inline thread_local char*      variable_buffer::S::tl_message_start_address    = nullptr;
template <> inline thread_local uint32_t*  variable_buffer::S::tl_pbytes_to_next_message   = nullptr;


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
    static constexpr std::size_t value =
        alignof(value_type) < alignof(variable_buffer)
            ? alignof(variable_buffer)
            : alignof(value_type);
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

    size_t size_bytes() const { return num_bytes; }
    size_t data_offset_bytes() const { return offset_in_bytes; }
    const void* data_address() const
    {
        return reinterpret_cast<const char*>(this) + offset_in_bytes;
    }
};


template <typename T>
struct is_variable_buffer_argument
    : std::integral_constant<
        bool,
        is_convertible<T, variable_buffer>::value ||
        is_base_of<
            variable_buffer,
            typename remove_cv<typename remove_reference<T>::type>::type
        >::value
    >
{};


namespace detail
{

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
    using decayed = typename std::remove_reference<T>::type;

    if constexpr (
        std::is_convertible_v<decayed, variable_buffer> &&
        has_value_type<decayed>::value)
    {
        using value_type = typename decayed::value_type;
        static_assert(
            std::is_trivially_copyable_v<value_type>,
            "variable_buffer payloads must be trivially copyable."
        );

        const size_t data_bytes = value.size() * sizeof(value_type);
        offset = align_up_size(offset, alignof(value_type)) + data_bytes;
        return accumulate_vb_size(offset, std::forward<Args>(args)...);
    }
    else {
        return accumulate_vb_size(offset, std::forward<Args>(args)...);
    }
}

} // namespace detail


template <typename MESSAGE_T, typename... Args>
size_t vb_size(Args&&... args)
{
    const size_t base_size = sizeof(MESSAGE_T);
    const size_t final_offset = detail::accumulate_vb_size(
        base_size,
        std::forward<Args>(args)...);

    assert(final_offset >= base_size);
    return final_offset - base_size;
}


using std::string;

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


class No
{
private:
    No(){}
    virtual int no()=0;
};

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
        "Unsupported message argument type. Provide a trivial standard-layout type, Sintra_message_element, or variable_buffer-compatible type."
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
    using decayed_type = typename remove_reference<T>::type;
    using bare_type = typename remove_cv<decayed_type>::type;
    using type = typename std::conditional<
        is_base_of<variable_buffer, bare_type>::value,
        decayed_type,
        typed_variable_buffer<bare_type>
    >::type;
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
    using arg_type = typename detail::message_args_nth_type<SEQ_T, I>::type;
    using transformed_type =
        typename transformer<typename remove_reference<arg_type>::type>::type;
    using aggregate_type =
        serializable_type_impl<SEQ_T, I + 1, J, Args..., transformed_type>;
    using type = typename aggregate_type::type;
    static constexpr bool has_variable_buffers = aggregate_type::has_variable_buffers ||
        !is_same<
            typename remove_reference<arg_type        >::type,
            typename remove_reference<transformed_type>::type
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
    const uint64_t magic                    = message_magic;

    // used by the serializer, set in constructor
    uint32_t bytes_to_next_message          = 0;

    union {
        // This is a cross-process type id. It is set in message constructor.
        type_id_type message_type_id        = invalid_type_id;

        // Only relevant in RPC return messages.
        type_id_type exception_type_id;
    };

    // Only instantiated in RPC messages, to associate the return messages with the original.
    // When this is set, the message_type_id becomes irrelevant.
    instance_id_type function_instance_id   = invalid_instance_id;

    // used by the serializer, set in communicators
    instance_id_type sender_instance_id     = invalid_instance_id;

    // used by the serializer, set in communicators
    instance_id_type receiver_instance_id   = invalid_instance_id;
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
    using body_type = T;
    using return_type = RT;
    using exporter = EXPORTER;

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
            static const type_id_type tid = get_type_id<Message>();
            return tid;
        }
    }


    template<
        typename... Args, typename = void,
        typename = enable_if_t< !args_require_varbuffer<Args...> >
    >
    Message(Args&& ...args)
    {
        bytes_to_next_message = sizeof(Message);

        void* body_ptr = static_cast<body_type*>(this);
        new (body_ptr) body_type{args...};

        assert(bytes_to_next_message < (message_ring_size / 8));

        message_type_id = id();
    }


    template<
        typename... Args,
        typename = enable_if_t< args_require_varbuffer<Args...> >
    >
    Message(Args&& ...args)
    {
        bytes_to_next_message = sizeof(Message);

        variable_buffer::S::tl_message_start_address = (char*) this;
        variable_buffer::S::tl_pbytes_to_next_message = &bytes_to_next_message;

        void* body_ptr = static_cast<body_type*>(this);
        new (body_ptr) body_type{args...};

        variable_buffer::S::tl_message_start_address = nullptr;
        variable_buffer::S::tl_pbytes_to_next_message = nullptr;

        assert(bytes_to_next_message < (message_ring_size / 8));

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
struct ring_payload_traits<Message<T, RT, ID, EXPORTER>> {
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


// [imak]: the "internal helpers" are due to a bug of MSVC's implementation of variadic macros.
// At the time of writing, MSVC2013 update 5 is still exhibiting the problem.
// I stole the original workaround from here:
// http://stackoverflow.com/questions/24836793/varargs-elem-macro-for-use-with-c/24837037#24837037


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
    typed_variable_buffer<typename remove_reference<T>::type> value;
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

    typename remove_reference<T>::type free_value;

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
    std::stringstream stream;
    stream << std::hex << id;
    if (occurrence > 0) {
        stream << "_occ" << std::dec << occurrence;
    }
    return prefix + stream.str();
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
        unsigned spin_count = 0;
        while (!m_reading_lock.compare_exchange_strong(f, true)) {
            f = false;
            detail::spin_pause();
            if (++spin_count >= 1024) {
                std::this_thread::yield();
                spin_count = 0;
            }
        }

        //m_reading_lock
        if (!m_reading) {
            m_reading_lock = false;
            return nullptr;
        }

        Message_prefix* ret = (Message_prefix*)m_range.begin;
        assert(ret->magic == message_magic);
        m_range.begin += ret->bytes_to_next_message;

        m_reading_lock = false;
        return ret;
    }

    sequence_counter_type get_message_reading_sequence() const
    {
        return reading_sequence() - (m_range.end - m_range.begin);
    }

public:
    const uint64_t  m_id;

protected:
    Range<char>     m_range;
};



struct Message_ring_W: public Ring_W<char>
{
    Message_ring_W(const string& directory, const string& prefix, uint64_t id, uint32_t occurrence = 0) :
        Ring_W(directory, get_base_filename(prefix, id, occurrence), message_ring_size),
        m_id(id)
    {}

    Message_ring_W(const Message_ring_W&) = delete;
    const Message_ring_W& operator = (const Message_ring_W&) = delete;

    void relay(const Message_prefix& msg)
    {
        write((const char*)&msg, msg.bytes_to_next_message);
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

