/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef __SINTRA_MESSAGE_H__
#define __SINTRA_MESSAGE_H__

#include "id_types.h"
#include "ipc_rings.h"
#include "utility.h"

#include <cstdint>
#include <type_traits>

#include <boost/fusion/container/vector.hpp>
#include <boost/type_index.hpp>



namespace sintra {


using std::enable_if_t;
using std::is_base_of;
using std::is_convertible;
using std::is_pod;
using std::is_same;
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

    variable_buffer(const variable_buffer& v)
    {
        *this = v;
    }

    template <typename TC, typename T = typename TC::iterator::value_type>
    variable_buffer(const TC& container);
};


template <> thread_local char*      variable_buffer::S::tl_message_start_address    = nullptr;
template <> thread_local uint32_t*  variable_buffer::S::tl_pbytes_to_next_message   = nullptr;


template <typename T>
struct typed_variable_buffer: protected variable_buffer
{
public:
    typed_variable_buffer() {}
    typed_variable_buffer(const T& v): variable_buffer(v) {}
    template <typename CT = typename T::iterator::value_type>
    operator T() const
    {
        assert(offset_in_bytes);
        assert((num_bytes % sizeof(CT)) == 0);

        CT* typed_data = (CT*)((char*)this+offset_in_bytes);
        size_t num_elements = num_bytes / sizeof(CT);
        return T(typed_data, typed_data + num_elements);
    }
};


inline
size_t vb_size()
{
    return 0;
}


template <
    typename T,
    typename = enable_if_t<
        !is_convertible< typename remove_reference<T>::type, variable_buffer>::value
    >,
    typename... Args
>
size_t vb_size(const T& v, Args&&... args);


template <
    typename T, typename = void,
    typename = enable_if_t<
        is_convertible< typename remove_reference<T>::type, variable_buffer>::value
    >,
    typename... Args
>
size_t vb_size(const T& v, Args&&... args);


template <
    typename T,
    typename /*= typename enable_if_t<
        !is_convertible< typename remove_reference<T>::type, variable_buffer>::value
    > */,
    typename... Args
>
size_t vb_size(const T& v, Args&&... args)
{
    auto ret = vb_size(args...);
    return ret;
}


template <
    typename T, typename /* = void*/,
    typename /* = typename enable_if_t<
        is_convertible< typename remove_reference<T>::type, variable_buffer>::value
    >*/,
    typename... Args
>
size_t vb_size(const T& v, Args&&... args)
{
    auto ret = v.size() * sizeof(typename T::iterator::value_type) + vb_size(args...);
    return ret;
}


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

template <
    typename T,
    bool =
        is_pod<T>::value ||
        is_base_of<Sintra_message_element, T>::value,
    bool = is_convertible<T, variable_buffer>::value
>
struct transformer
{
    using type = int; //PORT - THIS LINE IS WRONG
};


template <typename T, bool IRRELEVANT>
struct transformer<T, true, IRRELEVANT>
{
    using type = T;
};


template <typename T>
struct transformer<T, false, true>
{
    using type = typed_variable_buffer<T>;
};


template <typename SEQ_T, int I, int J, typename... Args>
struct serializable_type_impl;


template <typename SEQ_T, int I, typename... Args>
struct serializable_type_impl<SEQ_T, I, I, Args...>
{
    using type = typename boost::fusion::template vector<Args...>;
    static constexpr bool has_variable_buffers = false;
};


template <typename SEQ_T, int I, int J, typename... Args>
struct serializable_type_impl
{
    using arg_type = typename boost::fusion::result_of::value_at_c<SEQ_T, I>::type;
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
        boost::fusion::result_of::template size<SEQ_T>::value
    >,
    typename BASE = typename AGGREGATE::type
>
struct serializable_type: BASE
{
    using BASE::BASE;
    static constexpr bool has_variable_buffers = AGGREGATE::has_variable_buffers;
};


template<typename... Args>
constexpr bool args_require_varbuffer =
    serializable_type<typename boost::fusion::template vector<Args...> >::has_variable_buffers;


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
    // used by the serializer
    const uint64_t magic                    = message_magic;

    // used by the serializer, set in constructor
    uint32_t bytes_to_next_message          = 0;

    // this is a cross-process type id. It is set in message constructor. 
    type_id_type message_type_id            = invalid_type_id;

    // only instantiated in function messages, to associate the return messages to the original.
    // when this is set, the message_type_id is no longer relevant and will be ignored.
    instance_id_type function_instance_id   = invalid_instance_id;

    // used by the serializer, set in communicators
    instance_id_type sender_instance_id     = invalid_instance_id;

    // used by the serializer, set in communicators
    instance_id_type receiver_instance_id   = invalid_instance_id;

    // FIXME: Find a good way to exclude function_instance_id, as it is only relevant
    // to some types of messages and should be optional. There are many ways to do it, but none of
    // what has been considered and/or tried so far would leave the external interface of the
    // library completely unaffected, which should not happen.
};

template <typename T>
sintra::type_id_type get_type_id();

// This type only lives inside ring buffers.

template <
    typename T,
    typename RT = void,
    type_id_type ID = 0,
    typename Tag = void
>
struct Message: public Message_prefix, public T
{
    using body_type = T;
    using return_type = RT;


    static type_id_type id()
    {
        static type_id_type tid = ID ? make_type_id(ID) : get_type_id<Message>();
        return tid;
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

        assert(bytes_to_next_message < (message_ring_size / 2));

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

        assert(bytes_to_next_message < (message_ring_size / 2));

        message_type_id = id();
    }


    ~Message()
    {
    }
};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END MESSAGE //////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN SIGNAL MACROS //////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


// [imak]: the "internal helpers" are due to a bug of MSVC's implementation of variadic macros.
// At the time of writing, MSVC2013 update 5 is still exhibiting the problem.
// I stole the original workaround from here:
// http://stackoverflow.com/questions/24836793/varargs-elem-macro-for-use-with-c/24837037#24837037


/* internal helpers */
#define _VA_NARGS_GLUE(x, y) x y
#define _VA_NARGS_RETURN_COUNT(                                         \
    _1_ , _2_ , _3_ , _4_ , _5_ , _6_ , _7_ , _8_,                      \
    _9_ ,_10_, _11_, _12_, _13_, _14_, _15_, _16_, count, ...) count
#define _VA_NARGS_EXPAND(args) _VA_NARGS_RETURN_COUNT args
#define _VA_NARGS_COUNT_MAX16(...) _VA_NARGS_EXPAND((__VA_ARGS__,       \
    16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))

#define _VA_NARGS_OVERLOAD_MACRO2(name, count) name##count
#define _VA_NARGS_OVERLOAD_MACRO1(name, count) _VA_NARGS_OVERLOAD_MACRO2(name, count)
#define _VA_NARGS_OVERLOAD_MACRO(name,  count) _VA_NARGS_OVERLOAD_MACRO1(name, count)

/* expose for re-use */
#define VA_NARGS_CALL_OVERLOAD(name, ...) \
    _VA_NARGS_GLUE(_VA_NARGS_OVERLOAD_MACRO(name, _VA_NARGS_COUNT_MAX16(__VA_ARGS__)),(__VA_ARGS__))

/* overloads */
#define _VA_DEFINE_STRUCT1( v) \
    struct v { };
#define _VA_DEFINE_STRUCT2( v,a) \
    struct v { a; };
#define _VA_DEFINE_STRUCT3( v,a,b) \
    struct v { a;b; };
#define _VA_DEFINE_STRUCT4( v,a,b,c) \
    struct v { a;b;c; };
#define _VA_DEFINE_STRUCT5( v,a,b,c,d) \
    struct v { a;b;c;d; };
#define _VA_DEFINE_STRUCT6( v,a,b,c,d,e) \
    struct v { a;b;c;d;e; };
#define _VA_DEFINE_STRUCT7( v,a,b,c,d,e,f) \
    struct v { a;b;c;d;e;f; };
#define _VA_DEFINE_STRUCT8( v,a,b,c,d,e,f,g) \
    struct v { a;b;c;d;e;f;g; };
#define _VA_DEFINE_STRUCT9( v,a,b,c,d,e,f,g,h) \
    struct v { a;b;c;d;e;f;g;h; };
#define _VA_DEFINE_STRUCT10(v,a,b,c,d,e,f,g,h,i) \
    struct v { a;b;c;d;e;f;g;h;i; };
#define _VA_DEFINE_STRUCT11(v,a,b,c,d,e,f,g,h,i,j) \
    struct v { a;b;c;d;e;f;g;h;i;j; };
#define _VA_DEFINE_STRUCT12(v,a,b,c,d,e,f,g,h,i,j,k) \
    struct v { a;b;c;d;e;f;g;h;i;j;k; };
#define _VA_DEFINE_STRUCT13(v,a,b,c,d,e,f,g,h,i,j,k,l) \
    struct v { a;b;c;d;e;f;g;h;i;j;k;l; };
#define _VA_DEFINE_STRUCT14(v,a,b,c,d,e,f,g,h,i,j,k,l,m) \
    struct v { a;b;c;d;e;f;g;h;i;j;k;l;m; };
#define _VA_DEFINE_STRUCT15(v,a,b,c,d,e,f,g,h,i,j,k,l,m,n) \
    struct v { a;b;c;d;e;f;g;h;i;j;k;l;m;n; };
#define _VA_DEFINE_STRUCT16(v,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o) \
    struct v { a;b;c;d;e;f;g;h;i;j;k;l;m;n;o; };

/* reusable macro */
#define _DEFINE_STRUCT(...) VA_NARGS_CALL_OVERLOAD(_VA_DEFINE_STRUCT, __VA_ARGS__)



#define EXPORT_SIGNAL_BASE(name, idv, ...)                  \
    _DEFINE_STRUCT(_sm_body_type_##name, __VA_ARGS__)       \
    using name = Message<_sm_body_type_##name, void, idv>;

#define EXPORT_SIGNAL(name, ...)                            \
    EXPORT_SIGNAL_BASE(name, invalid_type_id, __VA_ARGS__)

#define EXPORT_SIGNAL_EXPLICIT(name, ...)                   \
    EXPORT_SIGNAL_BASE(name, sintra::detail::reserved_id::name, __VA_ARGS__)



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END SIGNAL MACROS/////////////////////////////////////////////////////
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
    bool = is_pod<T>::value
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
    bool C2 = is_pod<T>::value
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


struct Message_ring_R: protected Ring_R<message_ring_size, char>
{
    using Ring_R::Ring_R;
    using Ring_R::unblock;
    using Ring_R::m_id;

    // Returns a pointer to the buffer of the message
    // If there is no message to read, it blocks.
    Message_prefix* fetch_message()
    {
        // if all the messages in the reading buffer have been read
        if (m_reading_location == m_end_location) {
            // if this is not an uninitialised state
            if (m_reading_location) {
                // finalise the reading
                done_reading();
            }
            size_t bytes_remaining = 0;
            // start with a new reading buffer. this will block until there is something to read.
            auto rl = start_reading(&bytes_remaining);
            if (!rl) {
                return nullptr;
            }
            m_reading_location = rl;
            m_end_location = m_reading_location + bytes_remaining;
        }

        Message_prefix* ret = (Message_prefix*)m_reading_location;
        assert(ret->magic == message_magic);
        m_reading_location += ret->bytes_to_next_message;
        return ret;
    }

protected:

    char* m_end_location = nullptr;
    char* m_reading_location = nullptr;
};



struct Message_ring_W: public Ring_W<message_ring_size, char>
{
    using Ring_W::Ring_W;

    Message_ring_W(const Message_ring_W&) = delete;
    const Message_ring_W& operator = (const Message_ring_W&) = delete;

    void relay(const Message_prefix& msg)
    {
        write((const char*)&msg, msg.bytes_to_next_message);
        done_writing();
    }

};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END MESSAGE RINGS ////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////

} // namespace sintra

#endif
