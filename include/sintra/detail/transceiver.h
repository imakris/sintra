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

#ifndef SINTRA_TRANSCEIVER
#define SINTRA_TRANSCEIVER


#include "globals.h"
#include "id_types.h"
#include "message.h"
#include "spinlocked_containers.h"

#include <list>
#include <map>
#include <mutex>
#include <unordered_map>

#include <boost/bind.hpp>
#include <boost/type_traits.hpp>


namespace sintra {


using std::function;
using std::is_base_of;
using std::is_const;
using std::is_reference;
using std::list;
using std::map;
using std::mutex;
using std::remove_reference;
using std::string;
using std::unordered_map;




 //////////////////////////////////////////////////////////////////////////
///// BEGIN Typed_instance_id //////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


template <typename T>
struct Typed_instance_id
{
    instance_id_type    id;
    Typed_instance_id(const T& transceiver)
    {
        id = transceiver.m_instance_id;
    }


    Typed_instance_id(instance_id_type id_)
    {
        id = id_;
    }
};


template <typename T>
Typed_instance_id<T> make_typed_instance_id(const T& transceiver)
{
    return Typed_instance_id<T>(transceiver);
}


// This specialization applies to non-typed generic groups such as any_local etc.
template<>
struct Typed_instance_id<void>
{
    instance_id_type    id;
    Typed_instance_id(instance_id_type id_)
    {
        id = id_;
    }
};


inline
Typed_instance_id<void> make_untyped_instance_id(instance_id_type naked_instance_id)
{
    return Typed_instance_id<void>(naked_instance_id);
}


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Typed_instance_id ////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////



using handler_proc_registry_mid_record_type = 
    spinlocked_umap <
        instance_id_type,                                // sender
        list<function<void(const Message_prefix &)>>
    >;


using handler_registry_type =
    spinlocked_umap <
        type_id_type,                                    // message type
        handler_proc_registry_mid_record_type
    >;


struct Transceiver_base
{
    using Transceiver_type = Transceiver_base;

    template <typename = void>
    Transceiver_base(const string& name = "", uint64_t id = 0);

    ~Transceiver_base();

    inline
    instance_id_type instance_id() { return m_instance_id; }

    template <typename = void>
    bool assign_name(const string& name);

    template <typename = void>
    void destroy();

private:

    // Used when there is no infrastructure to initialize Transceiver_base, thus the caller
    // (Managed_process) calls the other constructor explicitly, when construction is possible.
    Transceiver_base(void*) {}

    template <typename = void>
    void construct(const string& name = "", uint64_t id = 0);

public:


    SINTRA_SIGNAL_EXPLICIT(instance_invalidated, instance_id_type instance_id);

    inline
    void instance_invalidated_handler(const instance_invalidated& msg);


    // this is not well defined. what is the question that this function trying to answer
    // and why? what is the point?
    // if there is ANY handler active for this type of message? local? remote? what?
    //bool is_handler_active(type_id_type message_type_id);

    // you have tried to activate a message handler of a message explicitly defined as local
    // (i.e. a message that is not supposed to cross process boundaries) on a sender of a
    // different process


    mutex m_handlers_mutex;



    list<function<void()>> m_deactivators;



    using handler_deactivator = std::function<void()>;


    template<typename MESSAGE_T, typename HT>
    handler_deactivator activate_impl(HT&& handler, instance_id_type sender_id);


    // A functor with an arbitrary non-message argument
    template<
        typename SENDER_T,
        typename FT,
        typename = decltype(&FT::operator()),  //must be functor
        typename FUNCTOR_ARG_T = decltype(resolve_single_functor_arg(*((FT*)0))),

        // prevent functors with message arguments from matching the template
        typename = enable_if_t<
            !is_base_of<
                Message_prefix,
                typename remove_reference<FUNCTOR_ARG_T>::type
            >::value
        >
    >
    handler_deactivator activate(
        const FT& internal_slot,
        Typed_instance_id<SENDER_T> sender_id);


    // A functor with a message argument
    template<
        typename SENDER_T,
        typename FT,
        typename = decltype(&FT::operator()),    //must be functor
        typename = void, // differentiate from the previous template
        typename FUNCTOR_ARG_T = decltype(resolve_single_functor_arg(*((FT*)0))),

        // only allow functors with message arguments to match the template
        typename = enable_if_t<
            is_base_of<
                Message_prefix,
                typename remove_reference<FUNCTOR_ARG_T>::type
            >::value
        >
    >
    handler_deactivator activate(
        const FT& internal_slot,
        Typed_instance_id<SENDER_T> sender_id);


    // A Transceiver_base member function with a message argument. The sender has to exist.
    template<
        typename SENDER_T,
        typename MESSAGE_T,
        typename OBJECT_T,
        typename RT = typename MESSAGE_T::return_type
    >
    handler_deactivator activate(
        RT(OBJECT_T::*v)(const MESSAGE_T&), 
        Typed_instance_id<SENDER_T> sender_id);


    // less safe convenience function
    template<
        typename MESSAGE_T,
        typename OBJECT_T,
        typename RT /* = typename MESSAGE_T::return_type*/
    >
    handler_deactivator
    activate(
        RT(OBJECT_T::* v)(const MESSAGE_T&),
        std::string sender_name);


    template <typename = void>
    void deactivate_all();

    
    template <
        typename MESSAGE_T,
        instance_id_type LOCALITY,
        typename SENDER_T,
        typename... Args>
    void send(Args&&... args);



 //////////////////////////////////////////////////////////////////////////
///// BEGIN RPC ////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


    // The following code was originally developed in MSVC2015 Update 2.
    // Please be aware that some design decisions aim to prevent compiler errors.
    // (for example, the resolution_dummy could have been avoided - the export function does
    // not have to be variadic, nor is it really necessary to have two copies of export to match
    // the const)

    template <
        typename MF,
        MF m,
        type_id_type ID,
        typename RT = decltype(resolve_rt(m)),
        typename OBJECT_T = decltype(resolve_object_type(m))
    >
    struct RPCTC_d // remote process call type container
    {
        using mf_type = MF;
        using r_type = RT;
        using o_type = OBJECT_T;
        const static MF mf() { return m; };
        enum { id = ID };
    };


    // Short note: It is fairly simple to change the message body from boost::fusion::vector to
    // std::tuple, to avoid yet another boost dependency, but be aware of the implications.
    // The implementation of fusion::vector is a plain struct. The tuple on the other hand will most
    // likely use recursive inheritance, which may produce different memory layout. In some cases,
    // the fusion::vector will be more compact. For example, the following
    // 
    //     std::cout <<
    //        sizeof(boost::fusion::vector<int, char, double, char, short, int>) << ' ' <<
    //        sizeof(std::tuple           <int, char, double, char, short, int>);
    //
    // on my system would print
    // 24 40


    template <
        typename RPCTC,   // <- the m in RPCTC makes it unique
        typename... Args
    >
    struct unique_message_body: serializable_type<boost::fusion::vector<Args...> >
    {
        using bfvec = serializable_type<boost::fusion::vector<Args...> >;
        using bfvec::bfvec;
    };



    template <typename RT>
    void warn_about_reference_return()
    {
        static_assert(!is_reference<RT>::value,
            "A function returning a reference cannot be exported for RPC. "
            "Read Sintra documentation for details.");
    }


    template <typename SEQ_T, int I, int J>
    struct warn_about_reference_args_impl {

        using arg_type = typename boost::fusion::result_of::value_at_c<SEQ_T, I>::type;

        static_assert(
            !is_reference<arg_type>::value ||
            is_const<typename remove_reference<arg_type>::type>::value,
            "A function with non-const reference arguments cannot be exported for RPC. "
            "Read Sintra documentation for details."
            );
        warn_about_reference_args_impl<SEQ_T, I + 1, J> dummy;
    };

    template <typename SEQ_T, int I>
    struct warn_about_reference_args_impl<SEQ_T, I, I> {};

    template <typename SEQ_T>
    void warn_about_reference_args()
    {
        warn_about_reference_args_impl<SEQ_T, 0, boost::fusion::result_of::size<SEQ_T>::value>();
    }


    // This is only for RPC, thus static. Since the handler maps to a class/struct function, all
    // instances of this class/struct use the same handler, which is just a function enclosing
    // a __thiscall to whichever member function was exported.
    inline
    static auto& get_rpc_handler_map();


    template <typename RPCTC>
    static auto& get_instance_to_object_map();


    template <typename R_MESSAGE_T, typename MESSAGE_T, typename OBJECT_T>
    static void finalize_rpc_write(
        R_MESSAGE_T* placed_msg, const MESSAGE_T& msg, const OBJECT_T* obj);


    template <
        typename RPCTC,
        typename MESSAGE_T,
        typename = void,
        typename = enable_if_t<is_same< typename RPCTC::r_type, void>::value>
    >
    static void rpc_handler(Message_prefix& untyped_msg);


    template <
        typename RPCTC,
        typename MESSAGE_T,
        typename = enable_if_t<!is_same< typename RPCTC::r_type, void>::value>
    >
    static void rpc_handler(Message_prefix& untyped_msg);


    template <
        typename RPCTC,
        typename RT,
        typename OBJECT_T,
        typename... FArgs,      // The argument types of the exported member function
        typename... RArgs        // The artument types used by the caller
    >
    static RT rpc(
        RT(OBJECT_T::* /*resolution dummy arg*/)(FArgs...),
        instance_id_type instance_id,
        RArgs&&... args);


    template <
        typename RPCTC,
        typename RT,
        typename OBJECT_T,
        typename... FArgs,
        typename... RArgs
    >
    static RT rpc(
        RT(OBJECT_T::* /*resolution dummy arg*/)(FArgs...) const,
        instance_id_type instance_id,
        RArgs&&... args);


    template <
        typename RPCTC,
        typename MESSAGE_T,
        typename... Args
    >
    static auto rpc_impl(instance_id_type instance_id, Args... args) -> typename RPCTC::r_type;



    struct Return_handler
    {
        function<void(const Message_prefix&)>   success_handler;
        function<void()>                        failure_handler;
        instance_id_type                        instance_id = 0;
    };


    inline
    instance_id_type activate_return_handler(const Return_handler &rh);

    inline
    void deactivate_return_handler(instance_id_type message_instance_id);



    template <typename RPCTC, typename MT>
    function<void()> export_rpc_impl();

    template <typename RPCTC, typename RT, typename OBJECT_T, typename... Args>
    function<void()> export_rpc(RT(OBJECT_T::* /*resolution dummy arg*/)(Args...) const);

    template <typename RPCTC, typename RT, typename OBJECT_T, typename... Args>
    function<void()> export_rpc(RT(OBJECT_T::* /*resolution dummy arg*/)(Args...));



#define SINTRA_RPC_IMPL(m, mfp, id)                                                             \
    using m ## _mftc = RPCTC_d<decltype(mfp), mfp, id>;                                         \
    Instantiator m ## _itt = export_rpc<m ## _mftc>(mfp);                                       \
                                                                                                \
    template<typename... Args>                                                                  \
    static auto rpc_ ## m (Resolvable_instance_id instance_id, Args&&... args)                  \
    {                                                                                           \
        return rpc<m ## _mftc>(mfp, instance_id, args...);                                      \
    }


#if 0 //defined(__GNUG__)

    // This is probably exploiting a bug of GCC, which circumvents the need for
    // TRANSCEIVER_PROLOGUE(name) inside any class deriving from Transceiver_base that uses RPC.

    #define SINTRA_RPC(m)                                                                       \
        typedef auto otr_ ## m ## _function() -> decltype(*this);                               \
        using otr_ ## m = std::remove_reference<decltype(((otr_ ## m ## _function*)0)())>::type;\
        SINTRA_RPC_IMPL(m, &otr_ ## m :: m, invalid_type_id)

    #define SINTRA_RPC_EXPLICIT(m)                                                              \
        typedef auto otr_ ## m ## _function() -> decltype(*this);                               \
        using otr_ ## m = std::remove_reference<decltype(((otr_ ## m ## _function*)0)())>::type;\
        SINTRA_RPC_IMPL(m, &otr_ ## m :: m, sintra::detail::reserved_id::m)

#elif 0 //(_MSC_VER >= 1900) // && (_MSC_VER < 1999)

    // And this is probably exploiting another bug, this time of of MSVC, which
    // also circumvents the need for TRANSCEIVER_PROLOGUE(name) inside the class definition.

    #define SINTRA_RPC(m)                                                                       \
        template <typename = void> void otr_ ## m ## _function () {}                            \
        using otr_ ## m = decltype ( resolve_object_type(& otr_ ## m ## _function<>) );         \
        SINTRA_RPC_IMPL(m, &otr_ ## m :: m, invalid_type_id)

    #define SINTRA_RPC_EXPLICIT(m)                                                              \
        template <typename = void> void otr_ ## m ## _function () {}                            \
        using otr_ ## m = decltype ( resolve_object_type(& otr_ ## m ## _function<>) );         \
        SINTRA_RPC_IMPL(m, &otr_ ## m :: m, sintra::detail::reserved_id::m)

#else

    // However, this is probably the right way to go, strictly abiding to C++,
    // which unfortunately requires TRANSCEIVER_PROLOGUE(name) in the class definition.

    #define SINTRA_RPC(m)                                                                       \
        SINTRA_RPC_IMPL(m, &Transceiver_type :: m, invalid_type_id)

    #define SINTRA_RPC_EXPLICIT(m)                                                              \
        SINTRA_RPC_IMPL(m, &Transceiver_type :: m, sintra::detail::reserved_id::m)

#endif

  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END RPC //////////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


private:

    // Handlers of return messages (i.e. messages delivering the results of function messages).
    // Note that the key is an instance_id_type, rather than a type_id_type.
    // Those message handlers identify with particular function message invocations, and their
    // lifetime ends with the end of the call.
    // They are assigned in pairs, to handle successful and failed calls.
    mutex m_return_handlers_mutex;
    spinlocked_umap<instance_id_type, Return_handler> m_active_return_handlers;


    handler_registry_type m_active_handlers;

    instance_id_type m_instance_id = 0;
    bool m_named = false;

    spinlocked_umap<string, instance_id_type>::iterator m_cache_iterator;


    friend struct Managed_process;
    friend struct Coordinator;
    friend struct Process_message_reader;

    template<typename>
    friend struct Typed_instance_id;
};



template<typename Derived_T>
struct Transceiver: Transceiver_base
{
    using Transceiver_base::Transceiver_base;

    using Transceiver_type = Derived_T;

    template <
        typename MESSAGE_T,
        typename SENDER_T = Transceiver_type,
        typename... Args>
    void emit_local(Args&&... args)
    {
        Transceiver_base::send<MESSAGE_T, any_local, SENDER_T>(std::forward<Args>(args)...);
    }

    template <
        typename MESSAGE_T,
        typename SENDER_T = Transceiver_type,
        typename... Args>
    void emit_remote(Args&&... args)
    {
        Transceiver_base::send<MESSAGE_T, any_remote, SENDER_T>(std::forward<Args>(args)...);
    }

    template <
        typename MESSAGE_T,
        typename SENDER_T = Transceiver_type,
        typename... Args>
    void emit_global(Args&&... args)
    {
        Transceiver_base::send<MESSAGE_T, any_local_or_remote, SENDER_T>(std::forward<Args>(args)...);
    }
};




} // sintra


#endif
