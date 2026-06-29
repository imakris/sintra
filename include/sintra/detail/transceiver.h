// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "id_types.h"
#include "messaging/message.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>


namespace sintra {

using std::condition_variable;
using std::deque;
using std::enable_if_t;
using std::function;
using std::is_base_of;
using std::is_const;
using std::is_reference;
using std::list;
using std::mutex;
using std::remove_reference;
using std::string;
using std::unordered_map;
using std::unordered_set;

namespace detail {

// Internal wait result used by Rpc_handle::get_until(...).
enum class Rpc_wait_status
{
    // This wait observed the handle in a terminal state.
    completed,

    // The deadline expired but the async RPC may still complete later unless
    // the caller abandons interest.
    deadline_exceeded,
};

// Internal lifecycle state for async RPC result retrieval.
// - cancelled: wait ended because process/coordinator teardown unblocked it
// - abandoned: caller locally dropped interest before a result arrived
enum class Rpc_completion_state
{
    // The async RPC is still waiting for a final outcome.
    pending,

    // A normal reply was received and can be retrieved with get().
    returned,

    // The remote side reported an exception; get() rethrows it locally.
    remote_exception,

    // The wait ended because teardown or lifeline loss unblocked outstanding RPCs.
    cancelled,

    // Caller-side interest was dropped locally before a result arrived.
    abandoned,
};

} // namespace detail

template<typename RT>
class Rpc_handle;

template<typename T>
struct Rpc_state;

template<typename T>
struct rpc_storage_type
{
    using type = T;
};

template<>
struct rpc_storage_type<void>
{
    using type = void_placeholder_t;
};


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


    Typed_instance_id(instance_id_type instance_id)
    {
        id = instance_id;
    }
};


// This specialization applies to non-typed generic groups such as any_local etc.
template<>
struct Typed_instance_id<void>
{
    instance_id_type    id;
    Typed_instance_id(instance_id_type instance_id)
    {
        id = instance_id;
    }
};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Typed_instance_id ////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////



struct Tn_type
{
    type_id_type   type_id = 0;
    string         name;

    bool operator==(const Tn_type&) const = default;
};


} // leaving sintra namespace temporarily, to define the hasher

namespace std
{
    template <>
    struct hash<sintra::Tn_type>
    {
        uint64_t operator()(const sintra::Tn_type& k) const
        {
            uint64_t res = 17;
            res = res * 31 + hash<sintra::type_id_type>()(k.type_id);
            res = res * 31 + hash<string>()(k.name);
            return res;
        }
    };
}


namespace sintra { // continue with sintra namespace


template <typename T>
struct Named_instance: std::string
{
    Named_instance(const std::string& rhs) : std::string(rhs) {}
    Named_instance(std::string&& rhs) : std::string(std::move(rhs)) {}
};


namespace detail
{

struct Handler_slot_state
{
    std::atomic<bool>     active{true};
    std::atomic<bool>     deactivation_claimed{false};
    std::atomic<uint32_t> invocations{0};
    std::mutex            invocation_mutex;
};


using handler_slot_key_t = const void*;


inline handler_slot_key_t handler_slot_key(
    const function<void(const Message_prefix&)>& handler) noexcept
{
    return std::addressof(handler);
}


inline unordered_map<handler_slot_key_t, std::shared_ptr<Handler_slot_state>>&
handler_slot_states()
{
    static unordered_map<handler_slot_key_t, std::shared_ptr<Handler_slot_state>> states;
    return states;
}


inline std::shared_ptr<Handler_slot_state> handler_slot_state_for(
    const function<void(const Message_prefix&)>& handler)
{
    auto& states = handler_slot_states();
    const auto key = handler_slot_key(handler);
    auto it = states.find(key);
    if (it != states.end()) {
        return it->second;
    }

    auto state = std::make_shared<Handler_slot_state>();
    states.emplace(key, state);
    return state;
}

} // namespace detail


using handler_proc_registry_mid_record_type =
    unordered_map<
        instance_id_type,                                // sender
        list<function<void(const Message_prefix&)>>
    >;


using handler_registry_type =
    unordered_map<
        type_id_type,                                    // message type
        handler_proc_registry_mid_record_type
    >;


// True when SenderT is "generic" (the any-sender placeholder void) or when
// SenderT is in MessageT's exporter hierarchy. Used by activate() overloads to
// reject handlers that filter on a sender unrelated to the message's exporter.
template <typename MessageT, typename SenderT>
inline constexpr bool can_handler_filter_on_sender_v =
    std::is_same_v<SenderT, void> ||
    std::is_base_of_v<typename MessageT::exporter, SenderT>;


struct Transceiver
{
    using Transceiver_type = Transceiver;

    template <typename = void>
    Transceiver(const string& name = "", uint64_t id = 0);
    template <typename = void>
    Transceiver(const char* name, uint64_t id = 0);

    ~Transceiver();

    inline
    instance_id_type instance_id() { return m_instance_id; }

    template <typename = void>
    bool assign_name(const string& name);

    template <typename = void>
    void destroy();

protected:

    // Used when there is no infrastructure to initialize Transceiver, thus the caller
    // (Managed_process) calls the other constructor explicitly, when construction is possible.
    Transceiver(void*) {}

    template <typename = void>
    void construct(const string& name = "", uint64_t id = 0);

public:

    SINTRA_MESSAGE_RESERVED(
        exception,
        message_string what
    )

    SINTRA_MESSAGE_RESERVED(
        deferral,
        instance_id_type new_fiid
    )

    list<function<void()>> m_deactivators;

    using handler_deactivator = std::function<void()>;

    template<typename MESSAGE_T, typename HT>
    handler_deactivator activate_impl(
        HT&&                                   handler,
        instance_id_type                       sender_id,
        decltype(m_deactivators)::iterator*    deactivator_it_ptr = nullptr);


    // Functor handler activation. Accepts both functors that take a raw value
    // (wrapped in Message<Enclosure<...>>) and functors that take a Message
    // directly; the body picks the right path via if constexpr on the deduced
    // argument type. SFINAE on &FT::operator() restricts this template to
    // functor-typed callables so the member-function-pointer overloads below
    // are not shadowed.
    template<
        typename SENDER_T,
        typename FT,
        typename = decltype(&FT::operator())
    >
    handler_deactivator activate(
        const FT&                              internal_slot,
        Typed_instance_id<SENDER_T>            sender_id,
        decltype(m_deactivators)::iterator*    deactivator_it_ptr = nullptr);


    // A Transceiver member function with a message argument. The sender has to exist.
    template<
        typename SENDER_T,
        typename MESSAGE_T,
        typename OBJECT_T,
        typename RT = typename MESSAGE_T::return_type
    >
    handler_deactivator activate(
        RT(OBJECT_T::*v)(const MESSAGE_T&),
        Typed_instance_id<SENDER_T>            sender_id,
        decltype(m_deactivators)::iterator*    deactivator_it_ptr = nullptr);


    // Any kind of slot (member or function) will be accepted here.
    // By default, the sender does not have to exist. If it does not exist, the
    // Coordinator is notified, and once the conditions are met, it will send a signal
    // back, to trigger a new activation attempt. This behaviour may be disabled,
    // by specifying 'true' in the first template parameter.
    template<
        bool sender_must_exist = false,
        typename SLOT_T,
        typename SENDER_T
    >
    handler_deactivator // unlike its overloads, this one does not return pointer
    activate(
        const SLOT_T& rcv_slot,
        Named_instance<SENDER_T> sender);


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
        bool MAY_BE_CALLED_DIRECTLY,
        bool IS_FIRE_AND_FORGET,
        typename RT = decltype(resolve_rt(m)),
        typename OBJECT_T = decltype(resolve_object_type(m))
    >
    struct RPCTC_d // remote process call type container
    {
        using mf_type = MF;
        using r_type  = RT;
        using o_type  = OBJECT_T;
        const static MF mf() { return m; };
        static constexpr type_id_type  id                     = ID;
        static constexpr bool          may_be_called_directly = MAY_BE_CALLED_DIRECTLY;
        static constexpr bool          is_fire_and_forget     = IS_FIRE_AND_FORGET;
    };


    // Short note: The message body is implemented with detail::message_args rather than std::tuple
    // to retain a plain-struct layout for the message arguments. This keeps
    // RPC argument packs as compact as possible in the ring buffer.


    template <
        typename RPCTC,   // <- the m in RPCTC makes it unique
        typename... Args
    >
    struct unique_message_body: serializable_type<detail::message_args<Args...>>
    {
        using body = serializable_type<detail::message_args<Args...>>;
        using body::body;
    };



    template <typename RT>
    void warn_about_reference_return()
    {
        static_assert(!is_reference<RT>::value,
            "A function returning a reference cannot be exported for RPC. "
            "Read Sintra documentation for details.");
    }


    template <typename SEQ_T, int I, int J>
    struct warn_about_reference_args_impl
    {

        using arg_type = typename detail::message_args_nth_type<SEQ_T, I>::type;

        static_assert(
            !std::is_reference_v<arg_type> ||
            std::is_const_v<std::remove_reference_t<arg_type>>,
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
        warn_about_reference_args_impl<SEQ_T, 0, detail::message_args_size<SEQ_T>::value>();
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
        R_MESSAGE_T*       placed_rep_msg,
        const MESSAGE_T&   req_msg,
        const OBJECT_T*    ref_obj,
        type_id_type       ex_tid,
        instance_id_type   fallback_sender_iid = invalid_instance_id);


    // This overload is meant to be directly used in special cases,
    // e.g. when returning to multiple recipients.
    // In this version the receiver and function instance ids are explicitly specified,
    // rather than deduced from the message, thus they override what might already be
    // specified in the message.
    template <typename R_MESSAGE_T, typename OBJECT_T>
    static void finalize_rpc_write(
        R_MESSAGE_T*       placed_rep_msg,
        instance_id_type   receiver_iid,
        instance_id_type   function_iid,
        const OBJECT_T*    ref_obj,
        type_id_type       ex_tid,
        instance_id_type   fallback_sender_iid = invalid_instance_id);

    template <
        typename RPCTC,
        typename MESSAGE_T
    >
    static void rpc_handler(Message_prefix& untyped_msg);


    // Unified RPC dispatcher: partial specialization on the member-function
    // pointer type carries the FArgs pack and the const/non-const-qualified
    // variant in one place, so the public rpc / rpc_async / export_rpc surface
    // does not need separate const overloads. Free-function pointers are not
    // supported here because the public RPC API is keyed to member functions
    // of a Transceiver-derived class.
    template <typename RPCTC, typename F>
    struct rpc_dispatcher
    {
        static_assert(std::is_member_function_pointer_v<F>,
            "rpc/rpc_async/export_rpc require a pointer-to-member-function as the "
            "resolution argument (e.g. &MyClass::my_method).");
    };

    template <typename RPCTC, typename RT, typename OBJECT_T, typename... FArgs>
    struct rpc_dispatcher<RPCTC, RT(OBJECT_T::*)(FArgs...)>
    {
        using message_type = Message<unique_message_body<RPCTC, FArgs...>, RT, RPCTC::id>;

        template <typename... RArgs>
        static RT sync(instance_id_type instance_id, RArgs&&... args)
        {
            // Keep FArgs explicit so RPC arguments are converted according to
            // the exported signature before vb_size() and message construction.
            return Transceiver::rpc_impl<RPCTC, message_type, FArgs...>(
                instance_id,
                std::forward<RArgs>(args)...);
        }

        template <typename... RArgs>
        static Rpc_handle<RT> async(instance_id_type instance_id, RArgs&&... args)
        {
            // See sync(): the message layout follows the exported signature,
            // not the exact call-site argument types.
            return Transceiver::rpc_async_impl<RPCTC, message_type, FArgs...>(
                instance_id,
                std::forward<RArgs>(args)...);
        }
    };

    template <typename RPCTC, typename RT, typename OBJECT_T, typename... FArgs>
    struct rpc_dispatcher<RPCTC, RT(OBJECT_T::*)(FArgs...) const>
        : rpc_dispatcher<RPCTC, RT(OBJECT_T::*)(FArgs...)>
    {};


    template <typename RPCTC, typename F, typename... RArgs>
    static auto rpc(F /*resolution dummy arg*/, instance_id_type instance_id, RArgs&&... args)
    {
        return rpc_dispatcher<RPCTC, F>::sync(instance_id, std::forward<RArgs>(args)...);
    }


    // Async-only public surface. Synchronous rpc_<method>() stays blocking and
    // reuses the same transported machinery internally without exposing a handle.
    template <typename RPCTC, typename F, typename... RArgs>
    static auto rpc_async(F /*resolution dummy arg*/, instance_id_type instance_id, RArgs&&... args)
    {
        return rpc_dispatcher<RPCTC, F>::async(instance_id, std::forward<RArgs>(args)...);
    }


    template <
        typename RPCTC,
        typename MESSAGE_T,
        typename... Args
    >
    static auto rpc_impl(instance_id_type instance_id, Args... args) -> typename RPCTC::r_type;

    template <
        typename RPCTC,
        typename MESSAGE_T,
        typename... Args
    >
    static auto rpc_async_impl(
        instance_id_type instance_id,
        Args... args) -> Rpc_handle<typename RPCTC::r_type>;



    struct Return_handler
    {
        function<void(const Message_prefix&)>   return_handler;
        function<void(const Message_prefix&)>   exception_handler;
        function<void(const Message_prefix&)>   deferral_handler;
        instance_id_type                        instance_id = 0;
    };


    template<typename=void>
    instance_id_type activate_return_handler(const Return_handler &rh);

    inline
    void deactivate_return_handler(instance_id_type function_instance_id);

    // useful for deferred
    void replace_return_handler_id(instance_id_type old_id, instance_id_type new_id);


    template <typename RPCTC, typename MT>
    function<void()> export_rpc_impl();

    template <typename RPCTC, typename F>
    function<void()> export_rpc(F /*resolution dummy arg*/)
    {
        using message_type = typename rpc_dispatcher<RPCTC, F>::message_type;
        return export_rpc_impl<RPCTC, message_type>();
    }



#define SINTRA_RPC_IMPL(m, mfp, id, mbcd, fire_and_forget)                                   \
    void rpc_assertion_##m() {                                                               \
        static_assert(std::is_same_v<                                                        \
            std::remove_pointer_t<decltype(this)>,                                           \
            Transceiver_type>,                                                               \
            "This Transceiver is not derived correctly."                                     \
        );                                                                                   \
    }                                                                                        \
    using m ## _mftc = RPCTC_d<decltype(mfp), mfp, id, mbcd, fire_and_forget>;               \
    static_assert(!fire_and_forget || std::is_void_v<typename m ## _mftc::r_type>,           \
        "Fire-and-forget functions (SINTRA_UNICAST) must return void");                      \
    sintra::Instantiator m ## _itt = export_rpc<m ## _mftc>(mfp);                            \
                                                                                             \
    template<typename... Args>                                                               \
    static auto rpc_ ## m (sintra::Resolvable_instance_id instance_id, Args&&... args)       \
    {                                                                                        \
        return rpc<m ## _mftc>(mfp, instance_id, std::forward<Args>(args)...);               \
    }                                                                                        \
                                                                                             \
    template<typename... Args>                                                               \
    static auto rpc_async_ ## m (sintra::Resolvable_instance_id instance_id, Args&&... args) \
    {                                                                                        \
        static_assert(!m ## _mftc::is_fire_and_forget,                                       \
            "rpc_async_<method>() is not available for fire-and-forget exports.");           \
        return rpc_async<m ## _mftc>(mfp, instance_id, std::forward<Args>(args)...);         \
    }

    
    // Exports a member function for RPC.
    #define SINTRA_RPC(m) \
        SINTRA_RPC_IMPL(m, &Transceiver_type :: m, sintra::invalid_type_id, true, false)

    // Exports a member function for RPC, provided that an ID is already reserved for a function
    // with that name. This is only meant to be used internally.
    #define SINTRA_RPC_EXPLICIT(m) \
        SINTRA_RPC_IMPL(m, &Transceiver_type :: m, (type_id_type)sintra::detail::reserved_id::m, true, false)

    // Exports a member function exclusively for RPC use. This implies that the function is
    // written in a way that makes no sense to call it directly. For example, this could be the
    // case if it performs some collective operation (i.e. an operation that involves multiple
    // processes, such as process barriers and scatter/gather), or returns asynchronously.
    // Functions which are meant to be used exclusively via RPC, will not take a direct-call
    // shortcut when called within the same process, but will instead use the RPC mechanism
    // in all cases.
    #define SINTRA_RPC_STRICT(m) \
        SINTRA_RPC_IMPL(m, &Transceiver_type :: m, sintra::invalid_type_id, false, false)

    // Exports a member function exclusively for RPC, provided that an ID is already reserved for
    // a function with that name. This is only meant to be used internally.
    #define SINTRA_RPC_STRICT_EXPLICIT(m) \
        SINTRA_RPC_IMPL(m, &Transceiver_type :: m, (type_id_type)sintra::detail::reserved_id::m, false, false)

    // Exports a void member function for fire-and-forget unicast messaging.
    // Similar to SINTRA_RPC, but does not send a reply message. Only works with void functions.
    // This provides efficient one-way messaging to specific transceiver instances without
    // the overhead of reply handling.
    #define SINTRA_UNICAST(m) \
        SINTRA_RPC_IMPL(m, &Transceiver_type :: m, sintra::invalid_type_id, true, true)

  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END RPC //////////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


private:

    class Rpc_execution_guard
    {
    public:
        Rpc_execution_guard() = default;
        explicit Rpc_execution_guard(Transceiver* owner);
        Rpc_execution_guard(Rpc_execution_guard&& other) noexcept;
        Rpc_execution_guard& operator=(Rpc_execution_guard&& other) noexcept;
        ~Rpc_execution_guard();

        explicit operator bool() const { return m_owner != nullptr; }

    private:
        Rpc_execution_guard(const Rpc_execution_guard&) = delete;
        Rpc_execution_guard& operator=(const Rpc_execution_guard&) = delete;

        Transceiver* m_owner = nullptr;
    };

    Rpc_execution_guard try_acquire_rpc_execution();
    void release_rpc_execution();
    void ensure_rpc_shutdown();

    // Atomic (under the per-RPCTC instance-map spinlock) lookup of the
    // RPC target object plus an attempt to acquire its execution guard.
    //
    // Holding the spinlock across both lookup AND try_acquire_rpc_execution
    // is the invariant that prevents the owner thread's ~Instantiator (which
    // erases the same map entry under this spinlock) from racing ahead and
    // letting ~Transceiver tear down m_rpc_lifecycle_mutex before we lock
    // it. Lock order is spinlock -> m_rpc_lifecycle_mutex;
    // ensure_rpc_shutdown and release_rpc_execution never take the spinlock,
    // so this cannot deadlock.
    //
    // Callers distinguish two failure modes via the returned struct:
    //   object == nullptr        -> instance_id not present in the map
    //   !guard                   -> object exists but is shutting down
    template <typename RPCTC>
    struct Rpc_target_handle
    {
        typename RPCTC::o_type*  object = nullptr;
        Rpc_execution_guard      guard;
    };

    template <typename RPCTC>
    static Rpc_target_handle<RPCTC> acquire_rpc_target(instance_id_type instance_id);

    // Handlers of return messages (i.e. messages delivering the results of function messages).
    // Note that the key is an instance_id_type, rather than a type_id_type.
    // Those message handlers identify with particular function message invocations, and their
    // lifetime ends with the end of the call.
    // They are assigned in pairs, to handle successful and failed calls.
    mutex                       m_return_handlers_mutex;
    unordered_map<instance_id_type, Return_handler>
                                m_active_return_handlers;
    deque<instance_id_type>     m_retired_return_handler_fifo;
    unordered_set<instance_id_type>
                                m_retired_return_handler_ids;
    static constexpr size_t     max_retired_return_handlers = 1024;

    mutex                       m_rpc_lifecycle_mutex;
    condition_variable          m_rpc_lifecycle_condition;
    size_t                      m_active_rpc_calls          = 0;
    bool                        m_accepting_rpc_calls       = true;
    bool                        m_rpc_shutdown_requested    = false;
    bool                        m_rpc_shutdown_complete     = false;


    instance_id_type            m_instance_id               = invalid_instance_id;
    bool                        m_published                 = false;

    string                      m_cache_name;


protected:

    // WARNING: this will be set when the Transceiver is published.
    // If it is not published, it will remain invalid.
    type_id_type                m_type_id                   = invalid_type_id;
    function<void()>            initialize_type_id          = [this]()
    {
        m_type_id = get_type_id<Transceiver_type>();
    };


    friend struct Managed_process;
    friend struct Coordinator;
    friend struct Process_message_reader;

    template<typename>
    friend struct Typed_instance_id;

    template<typename , typename>
    friend struct Derived_transceiver;
};



struct empty_struct_t {};


// Second or higher level inheritance (i.e. inherits from a parent
// who is already derived from Transceiver)
template<typename Derived_T, typename Parent=void>
struct Derived_transceiver: Parent
{
    //using Transceiver::Transceiver;

    Derived_transceiver():
        base( *static_cast<Transceiver*>(static_cast<Derived_T*>(this)))
    {
    }

    using Transceiver_type = Derived_T;

    /// @name Typed Message Emission
    /// These methods send typed protocol messages from this transceiver.
    /// The message type is specified explicitly as a template parameter.
    ///
    /// For simple data broadcasting where the message type doesn't matter,
    /// use the Maildrop streaming API instead: world() << value, local() << value,
    /// or remote() << value. Those wrap values in Message<Enclosure<T>> automatically.
    /// @{

    /// Send a typed message to local recipients only (same process).
    template <
        typename MESSAGE_T,
        typename SENDER_T = Transceiver_type,
        typename... Args>
        void emit_local(Args&&... args)
    {
        base.send<MESSAGE_T, any_local, SENDER_T>(std::forward<Args>(args)...);
    }

    /// Send a typed message to remote recipients only (other processes).
    template <
        typename MESSAGE_T,
        typename SENDER_T = Transceiver_type,
        typename... Args>
        void emit_remote(Args&&... args)
    {
        base.send<MESSAGE_T, any_remote, SENDER_T>(std::forward<Args>(args)...);
    }

    /// Send a typed message to all recipients (local and remote).
    template <
        typename MESSAGE_T,
        typename SENDER_T = Transceiver_type,
        typename... Args>
        void emit_global(Args&&... args)
    {
        base.send<MESSAGE_T, any_local_or_remote, SENDER_T>(std::forward<Args>(args)...);
    }

    /// @}


    static Named_instance<Transceiver_type> named_instance(const std::string& name)
    {
        return name;
    }

private:
    Transceiver& base;

    int type_initializer()
    {
        base.initialize_type_id = [this]() {
            base.m_type_id = get_type_id<Transceiver_type>();
        };
        return 0;
    }
    int itidi = type_initializer(); // initialize the initialize_type_id
};



// First level inheritance, after Transceiver (thus has to inherit from Transceiver)
template<typename Derived_T>
struct Derived_transceiver<Derived_T, void>: Transceiver, Derived_transceiver<Derived_T, empty_struct_t>
{
    using Transceiver::Transceiver;
    using Transceiver_type = Derived_T;

private:

    int type_initializer()
    {
        initialize_type_id = [this]() {
            m_type_id = get_type_id<Transceiver_type>();
        };
        return 0;
    }
    int itidi = type_initializer(); // initialize the initialize_type_id
};


// Typed exception: thrown when an RPC is unblocked/cancelled (e.g., coordinator loss)
struct rpc_cancelled : std::runtime_error
{
    explicit rpc_cancelled(const char* what_arg) : std::runtime_error(what_arg) {}
};

// Typed exception: thrown when an RPC cannot be delivered because the target
// is unpublished, shutting down, or its process is gone. Distinct from
// rpc_cancelled (which signals teardown of the *caller's* runtime). Inherits
// from std::runtime_error, so a catch (const std::runtime_error&) handler
// also matches.
struct rpc_unavailable : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Typed exception: thrown when a local bounded wait gives up on a pending RPC.
struct rpc_timeout : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Outstanding_rpc_control
{
    mutex               keep_waiting_mutex;
    condition_variable  keep_waiting_condition;

    // this is set before the rpc is written to the ring, in rpc_impl.
    // It is meant to identify the remote instance, in case it
    // crashes, in order to unblock the local caller.
    instance_id_type    remote_instance = invalid_instance_id;

    bool                keep_waiting = true;
    bool                cancelled = false;
    bool                abandoned = false;
    bool                cleaned_up = false;
};

namespace detail { namespace test_hooks {

// Stage name shared with tests that exercise the get_until timeout/abandon
// race deterministically.
inline constexpr const char* k_stage_rpc_get_until_deadline_before_abandon =
    "rpc_handle/get_until/deadline_before_abandon";

#if defined(SINTRA_ENABLE_TEST_HOOKS)
using Rpc_get_until_stage_callback = void (*)(const char* stage);
inline std::atomic<Rpc_get_until_stage_callback> s_rpc_get_until_stage{nullptr};
#endif

}} // namespace detail::test_hooks

template<typename RT>
class Rpc_handle
{
public:
    // Public async RPC result handle returned by rpc_async_<method>().
    // Synchronous rpc_<method>() remains a blocking surface and only reuses the
    // same transported state internally.
    // Default-constructed handles are empty; moved-from handles should only be
    // destroyed or assigned a new state.
    Rpc_handle() = default;
    Rpc_handle(const Rpc_handle&) = delete;
    Rpc_handle(Rpc_handle&&) noexcept = default;
    Rpc_handle& operator=(const Rpc_handle&) = delete;
    Rpc_handle& operator=(Rpc_handle&& other) noexcept;

    // Destroying a pending handle drops caller-side interest without blocking.
    ~Rpc_handle();

    // Wait until deadline, then return the value or rethrow the terminal failure.
    // If the deadline expires and this handle wins the local drop race, throws rpc_timeout.
    // If completion wins that race, preserves get()'s outcome.
    template<typename Clock, typename Duration>
    auto get_until(const std::chrono::time_point<Clock, Duration>& deadline) -> RT;

    // Block if needed, then return the value or rethrow the terminal failure.
    // - returned: yields the reply value
    // - remote_exception: rethrows the remote exception locally
    // - cancelled: throws rpc_cancelled
    // - abandoned: throws std::runtime_error
    auto get() const -> RT;

private:
    using storage_type = typename rpc_storage_type<RT>::type;
    using state_type   = Rpc_state<storage_type>;

    explicit Rpc_handle(std::shared_ptr<state_type> state);

    void wait() const;

    template<typename Clock, typename Duration>
    detail::Rpc_wait_status wait_until(
        const std::chrono::time_point<Clock, Duration>& deadline) const;

    detail::Rpc_completion_state state() const;

    bool abandon();

    std::shared_ptr<state_type> m_state;

    friend struct Transceiver;
};



} // sintra
