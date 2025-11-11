// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "process/coordinator.h"
#include "process/managed_process.h"
#include "transceiver.h"

#include <cassert>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "exception_conversions.h"
#include "exception_conversions_impl.h"
#include "messaging/process_message_reader.h"


namespace sintra {


using std::enable_if;
using std::function;
using std::make_pair;
using std::remove_reference;
using std::runtime_error;
using std::shared_ptr;
using std::string;
using std::is_same_v;
using std::is_base_of_v;
using std::unique_lock;



template <typename/* = void*/>
Transceiver::Transceiver(const string& name/* = ""*/, uint64_t instance_id/* = 0*/)
{
    construct(name, instance_id);
}



inline
Transceiver::~Transceiver()
{
    destroy();
}



template <typename/* = void*/>
void Transceiver::construct(const string& name/* = ""*/, uint64_t instance_id/* = 0*/)
{
    if (!s_mproc) {
        throw runtime_error("Failed to create a Transceiver. Sintra is possibly not initialized.");
    }

    {
        unique_lock<mutex> lock(m_rpc_lifecycle_mutex);
        m_accepting_rpc_calls = true;
        m_active_rpc_calls = 0;
        m_rpc_shutdown_requested = false;
        m_rpc_shutdown_complete = false;
    }

    m_instance_id = instance_id ? instance_id : make_instance_id();

    if (m_instance_id == invalid_instance_id) {
        // this is practically unreachable
        throw runtime_error("Failed to create a Transceiver instance id.");
    }

    s_mproc->m_local_pointer_of_instance_id[m_instance_id] = this;

    // A transceiver instance may as well not have a name (in which case, name lookups fail).
    if (!name.empty()) {
        if (!assign_name(name)) {
            throw runtime_error("Transceiver name assignment failed.");
        }
    }
    /*
    activate(
        &Transceiver::instance_invalidated_handler,
        Typed_instance_id<void>(any_local_or_remote) );
    */
}



inline
Transceiver::Rpc_execution_guard::Rpc_execution_guard(Transceiver* owner):
    m_owner(owner)
{}


inline
Transceiver::Rpc_execution_guard::Rpc_execution_guard(Rpc_execution_guard&& other) noexcept:
    m_owner(other.m_owner)
{
    other.m_owner = nullptr;
}


inline
Transceiver::Rpc_execution_guard&
Transceiver::Rpc_execution_guard::operator=(Rpc_execution_guard&& other) noexcept
{
    if (this != &other) {
        if (m_owner) {
            m_owner->release_rpc_execution();
        }
        m_owner = other.m_owner;
        other.m_owner = nullptr;
    }
    return *this;
}


inline
Transceiver::Rpc_execution_guard::~Rpc_execution_guard()
{
    if (m_owner) {
        m_owner->release_rpc_execution();
    }
}


inline
Transceiver::Rpc_execution_guard
Transceiver::try_acquire_rpc_execution()
{
    unique_lock<mutex> lock(m_rpc_lifecycle_mutex);
    if (!m_accepting_rpc_calls) {
        return {};
    }

    ++m_active_rpc_calls;
    return Rpc_execution_guard(this);
}


inline
void
Transceiver::release_rpc_execution()
{
    bool should_notify;
    {
        unique_lock<mutex> lock(m_rpc_lifecycle_mutex);
        assert(m_active_rpc_calls);

        --m_active_rpc_calls;
        should_notify = m_rpc_shutdown_requested && m_active_rpc_calls == 0;
        // Lock automatically released by scope exit
    }

    if (should_notify) {
        m_rpc_lifecycle_condition.notify_all();
    }
}


inline
void
Transceiver::stop_accepting_rpc_calls_and_wait()
{
    ensure_rpc_shutdown();
}


inline
void
Transceiver::ensure_rpc_shutdown()
{
    unique_lock<mutex> lock(m_rpc_lifecycle_mutex);

    if (m_rpc_shutdown_complete) {
        return;
    }

    if (!m_rpc_shutdown_requested) {
        m_rpc_shutdown_requested = true;
        m_accepting_rpc_calls = false;
        m_rpc_lifecycle_condition.wait(lock, [&]() { return m_active_rpc_calls == 0; });
        m_rpc_shutdown_complete = true;
        // Notify while still holding the lock to prevent race conditions
        m_rpc_lifecycle_condition.notify_all();
    }
    else {
        // Another thread is performing shutdown - wait for it to complete
        m_rpc_lifecycle_condition.wait(lock, [&]() { return m_rpc_shutdown_complete; });
    }
}


template <typename/* = void*/>
bool Transceiver::assign_name(const string& name)
{
    initialize_type_id();
    if (Coordinator::rpc_publish_transceiver(s_coord_id, m_type_id, m_instance_id, name)) {
        m_published = true;

        if (!s_coord) {
            auto cache_entry = make_pair(name, m_instance_id);
            auto rvp = s_mproc->m_instance_id_of_assigned_name.insert(cache_entry);
            assert(rvp.second == true);
            m_cache_iterator = rvp.first;
        }
        return true;
    }
    return false;
}



template <typename/* = void*/>
void Transceiver::destroy()
{
    bool no_readers = true;
    if (s_mproc && s_coord_id) {
        std::shared_lock<std::shared_mutex> readers_lock(s_mproc->m_readers_mutex);
        no_readers = s_mproc->m_readers.empty();
    }

    if (!s_mproc || !s_coord_id || no_readers) {
        // If the process is not running, there is nothing to facilitate the basic
        // functionality of a Transceiver object. This can happen if the process object is
        // itself being destroyed.
        return;
    }

    stop_accepting_rpc_calls_and_wait();

    if (this != s_mproc) {
        deactivate_all();
    }

    if (m_published) {

        // If the message ring threads are not running, attempting rpc would deadlock,
        // which must be prevented. This is a likely scenario on an emergency exit.
        if (s_mproc->m_communication_state <= Managed_process::COMMUNICATION_PAUSED) {
            if (s_coord) {
                s_coord->unpublish_transceiver_notify(m_instance_id);
            }
            else {
                s_mproc->emit_remote<Managed_process::unpublish_transceiver_notify>(m_instance_id);
            }
        }
        else {
            // During shutdown, the coordinator can already be draining or gone.
            // Treat RPC failure as non-fatal in that case - the coordinator will
            // clean up when it detects the process disconnect.
            bool success = false;
            try {
                success = Coordinator::rpc_unpublish_transceiver(s_coord_id, m_instance_id);
            }
            catch (...) {
                // Coordinator already shutting down or unreachable - ignore
                success = false;
            }
            // In normal operation, unpublish should succeed. During shutdown races,
            // it's acceptable for this to fail silently.
            (void)success;
        }

        m_published = false;

        // if the coordinator is local, it would be deleted already in the unpublish call
        if (!s_coord) {
            s_mproc->m_instance_id_of_assigned_name.erase(m_cache_iterator);
        }
    }

    // Always remove from local pointer map (for both published and unpublished transceivers)
    // to prevent stale pointers when unpublish_all_transceivers() is called during finalize().
    if (m_instance_id != s_coord_id) {
        s_mproc->m_local_pointer_of_instance_id.erase(m_instance_id);
    }
}


template<typename MESSAGE_T, typename HT>
Transceiver::handler_deactivator
Transceiver::activate_impl(
    HT&& handler,
    instance_id_type sender_id,
    decltype(m_deactivators)::iterator* deactivator_it_ptr)
{
    // an invalid instance must never be passed to this function (must be checked earlier)
    assert(sender_id != invalid_instance_id);

    auto message_type_id = MESSAGE_T::id();

    using Handler_function = function<typename MESSAGE_T::return_type(const MESSAGE_T&)>;
    Handler_function normalized_handler(std::forward<HT>(handler));

    auto wrapper_lambda = [handler_fn = std::move(normalized_handler)](const Message_prefix& prefix) mutable
    {
        handler_fn(static_cast<const MESSAGE_T&>(prefix));
    };

    function<void(const Message_prefix&)> wrapper(wrapper_lambda);

    lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);

    auto& ms  = s_mproc->m_active_handlers[message_type_id];
    list<function<void(const Message_prefix &)>>::iterator mid_sid_it;

    auto  msm_it = ms.find(sender_id);
    if (msm_it == ms.end()) {

        // There was no record for this sender_id, thus we have to make one.

        list<function<void(const Message_prefix&)>> handler_list;
        handler_list.emplace_back(wrapper);
        msm_it = ms.emplace(sender_id, std::move(handler_list)).first;
        mid_sid_it = msm_it->second.begin();
    }
    else {
        mid_sid_it = msm_it->second.emplace(msm_it->second.end(), wrapper);
    }

    decltype(m_deactivators)::iterator deactivator_it;

    if (!deactivator_it_ptr) {
        // emplace a default object in the deactivators, to obtain an iterator
        // which is needed in the lambda below.
        m_deactivators.emplace_back();
        deactivator_it = std::prev(m_deactivators.end());
    }
    else {
        deactivator_it = *deactivator_it_ptr;
    }

    *deactivator_it = [=, &ms] () {
        lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);
        msm_it->second.erase(mid_sid_it);
        if (msm_it->second.empty()) {
            ms.erase(msm_it);
        }
        m_deactivators.erase(deactivator_it);
    };

    return m_deactivators.back();
}



// A functor with an arbitrary non-message argument
template<
    typename SENDER_T,
    typename FT,
    typename  /* = decltype(&FT::operator())*/,  //must be functor
    typename FUNCTOR_ARG_T /* = decltype(resolve_single_functor_arg(*((FT*)0)))*/,

    // prevent functors with message arguments from matching the template
    typename /* = typename enable_if_t<
        !std::is_base_of<
            Message_prefix,
            typename std::remove_reference<FUNCTOR_ARG_T>::type
        >::value
    >*/
>
typename Transceiver::handler_deactivator
Transceiver::activate(
    const FT& internal_slot,
    Typed_instance_id<SENDER_T> sender_id,
    decltype(m_deactivators)::iterator* deactivator_it_ptr)
{
    // this is an arbitrary functor, quite possibly a lambda. The first and only argument
    // should be matched here. If it fails, the function is incompatible to its purpose.
    using arg_type = decltype(resolve_single_functor_arg(internal_slot));

    // Slots would only take messages of specific, cross-process identifiable type, thus the
    // 'internal_slot' is not really a slot. We need to make a proper slot and enclose the
    // functor call in it.

    using MT = Message<Enclosure<arg_type>>;
    function<void(const MT &msg)> handler = [internal_slot](const MT &msg) -> auto
    {
        return internal_slot(msg.get_value());
    };

    return activate_impl<MT>(handler, sender_id.id, deactivator_it_ptr);
}



// A functor with a message argument
template<
    typename SENDER_T,
    typename FT,
    typename  /* = decltype(&FT::operator())*/,    //must be functor
    typename  /* = void*/, // differentiate from the previous template
    typename FUNCTOR_ARG_T /* = decltype(resolve_single_functor_arg(*((FT*)0)))*/,

    // only allow functors with message arguments to match the template
    typename /* = typename enable_if_t<
        is_base_of<
            Message_prefix,
            typename remove_reference<FUNCTOR_ARG_T>::type
        >::value
    >*/
>
typename Transceiver::handler_deactivator
Transceiver::activate(
    const FT& internal_slot,
    Typed_instance_id<SENDER_T> sender_id,
    decltype(m_deactivators)::iterator* deactivator_it_ptr)
{
    // the given slot is already a functor taking a message argument, thus there is no need for
    // inclusion into a lambda. There is however the need to convert to function, since
    // we don't know what kind of functor it is.

    using arg_type = decltype(resolve_single_functor_arg(internal_slot));
    using MT = typename remove_reference<arg_type>::type;
    auto handler = static_cast<function<typename MT::return_type(const arg_type&)>>(internal_slot);

    constexpr bool sender_capability =
        is_same_v    < SENDER_T, void > ||                   // generic sender (e.g. any_local)
        is_base_of_v < typename MT::exporter, SENDER_T >;    // the exporter is sender's base

    static_assert(sender_capability, "This type of sender cannot send the type of messages "
        "handled by the specified handler.");

    return activate_impl<MT>(handler, sender_id.id, deactivator_it_ptr);
}



// A Transceiver member function with a message argument. The sender has to exist.
template<
    typename SENDER_T,
    typename MESSAGE_T,
    typename OBJECT_T,
    typename RT /* = typename MESSAGE_T::return_type*/
>
Transceiver::handler_deactivator
Transceiver::activate(
    RT(OBJECT_T::*v)(const MESSAGE_T&),
    Typed_instance_id<SENDER_T> sender_id,
    decltype(m_deactivators)::iterator* deactivator_it_ptr)
{
    auto handler = function<typename MESSAGE_T::return_type(const MESSAGE_T&)>(
        [this, v](const MESSAGE_T& message) {
            return (static_cast<OBJECT_T*>(this)->*v)(message);
        });

    constexpr bool sender_capability =
        is_same_v    < SENDER_T, void > ||                        // generic sender (e.g. any_local)
        is_base_of_v < typename MESSAGE_T::exporter, SENDER_T >;  // the exporter is sender's base

    static_assert(sender_capability, "This type of sender cannot send the type of messages "
        "handled by the specified handler.");

    return activate_impl<MESSAGE_T>(handler, sender_id.id, deactivator_it_ptr);
}



// Any kind of slot (member or function) will be accepted here.
// By default, the sender does not have to exist. If it does not exist, the
// Coordinator is notified, and once the conditions are met, it will send a signal
// back, to trigger a new activation attempt. This behaviour may be disabled,
// by specifying 'true' in the first template parameter.
template<
    bool sender_must_exist /* = false */,
    typename SLOT_T,
    typename SENDER_T
>
Transceiver::handler_deactivator
Transceiver::activate(
    const SLOT_T& rcv_slot,
    Named_instance<SENDER_T> sender)
{
    lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex); //obtain activation lock

    // make an entry in the deactivators list first - it must be captured below
    m_deactivators.emplace_back();
    auto it = std::prev(m_deactivators.end());

    // make a lambda that will perform the activation, and will also replace
    // the deactivator with the one returned by activate_impl
    auto wrapped_activation = [&, rcv_slot, sender, it]() mutable {

        lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);

            auto iid = Typed_instance_id<SENDER_T>(get_instance_id(std::move(sender) ) );
        
            // the enclosing lambda is guaranteed to have been triggered by a publish event
            // which means that the transceiver exists
            assert (iid.id != invalid_instance_id);

            // activate and replace the old deactivator with the one returned by activate_impl
            // note the last argument, which specifies a place in the deactivation list, which
            // prevents allocating a new one.
            activate(rcv_slot, iid, &it);

            // a function with the same effect as coa_abort (below) is called
            // immediately after this lambda, by its caller
        };

    // Let the activation happen when a transceiver with matching name and type becomes available.
    auto coa_abort = s_mproc->call_on_availability(sender, wrapped_activation);

    // Until the actual activation happens, this lambda will serve as a temporary deactivator.
    // It only aborts the call on availability by calling its aborter, and also removes
    // itself from the deactivator list
    *it = [=]() {

        lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);

        coa_abort(); // this will also remove the coa request from the corresponding list
        m_deactivators.erase(it);
    };

    return m_deactivators.back();
}



template <typename /* = void*/>
void Transceiver::deactivate_all()
{
    while (!m_deactivators.empty())
        m_deactivators.back()();
}



template <
    typename MESSAGE_T,
    instance_id_type LOCALITY,
    typename SENDER_T,
    typename... Args>
void Transceiver::send(Args&&... args)
{
    static_assert(
        std::is_base_of_v<Message_prefix, MESSAGE_T>,
        "Attempting to send something that is not a message.");

    constexpr bool sender_capability =
        is_same_v    < typename MESSAGE_T::exporter, void     > ||
        is_base_of_v < typename MESSAGE_T::exporter, SENDER_T >;

    static_assert(sender_capability, "This type of sender cannot send messages of this type.");

    static auto once = MESSAGE_T::id();
    (void)(once); // suppress unused variable warning

    MESSAGE_T* msg = s_mproc->m_out_req_c->write<MESSAGE_T>(vb_size<MESSAGE_T>(args...), args...);
    msg->sender_instance_id = m_instance_id;
    msg->receiver_instance_id = LOCALITY;
    s_mproc->m_out_req_c->done_writing();
}


 //////////////////////////////////////////////////////////////////////////
///// BEGIN RPC ////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//



// This is only for RPC, thus static. Since the handler maps to a class/struct function, all
// instances of this class/struct use the same handler, which is just a function enclosing
// a __thiscall to whichever member function was exported.
inline
auto& Transceiver::get_rpc_handler_map()
{
    static spinlocked_umap<type_id_type, void(*)(Message_prefix&)> message_id_to_handler;
    return message_id_to_handler;
}



template <typename RPCTC>
auto& Transceiver::get_instance_to_object_map()
{
    static spinlocked_umap<instance_id_type, typename RPCTC::o_type*> instance_to_object;
    return instance_to_object;
}


template <typename R_MESSAGE_T, typename MESSAGE_T, typename OBJECT_T>
void Transceiver::finalize_rpc_write(
    R_MESSAGE_T* placed_rep_msg,
    const MESSAGE_T& req_msg,
    const OBJECT_T* ref_obj,
    type_id_type ex_tid,
    instance_id_type fallback_sender_iid)
{
    finalize_rpc_write(
        placed_rep_msg,
        req_msg.sender_instance_id,
        req_msg.function_instance_id,
        ref_obj,
        ex_tid,
        fallback_sender_iid);
}

template <typename R_MESSAGE_T, typename OBJECT_T>
void Transceiver::finalize_rpc_write(
    R_MESSAGE_T* placed_rep_msg,
    instance_id_type receiver_iid,
    instance_id_type function_iid,
    const OBJECT_T* ref_obj,
    type_id_type ex_tid,
    instance_id_type fallback_sender_iid)
{
    instance_id_type sender_iid = invalid_instance_id;

    if (ref_obj) {
        sender_iid = ref_obj->m_instance_id;
    }
    else
    if (fallback_sender_iid != invalid_instance_id) {
        sender_iid = fallback_sender_iid;
    }
    else
    if (s_tl_current_message) {
        sender_iid = s_tl_current_message->receiver_instance_id;
    }
    else
    if (s_mproc) {
        sender_iid = s_mproc->m_instance_id;
    }

    if (sender_iid == invalid_instance_id) {
        assert(s_mproc);
        sender_iid = s_mproc->m_instance_id;
    }

    placed_rep_msg->sender_instance_id   = sender_iid;
    placed_rep_msg->receiver_instance_id = receiver_iid;
    placed_rep_msg->function_instance_id = function_iid;
    placed_rep_msg->message_type_id      = ex_tid;

    assert(placed_rep_msg->receiver_instance_id != 0);

    s_mproc->m_out_rep_c->done_writing();
}


template <typename T>
struct Void_filter
{
    std::function<T()> f;
    T result = {};
    void call() { result = f(); }
};

template <>
struct Void_filter<void_placeholder_t>
{
    std::function<void()> f;
    void_placeholder_t result = {};
    void call() { f(); }
};


template<typename T> struct unvoid       { using type = T;                  };
template<>           struct unvoid<void> { using type = void_placeholder_t; };


template <
    typename RPCTC,
    typename MESSAGE_T
>
void Transceiver::rpc_handler(Message_prefix& untyped_msg)
{
    using r_type = typename unvoid<typename RPCTC::r_type>::type;

    MESSAGE_T& msg = (MESSAGE_T&)untyped_msg;
    typename RPCTC::o_type* obj = nullptr;

    // Hold spinlock while accessing the iterator to prevent use-after-invalidation
    {
        auto scoped_map = get_instance_to_object_map<RPCTC>().scoped();
        auto it = scoped_map.get().find(untyped_msg.receiver_instance_id);
        if (it != scoped_map.get().end()) {
            obj = it->second;
        }
    }

    auto send_unavailable_response = [&](const std::string& reason)
    {
        auto* placed_msg = s_mproc->m_out_rep_c->write<exception>(vb_size<exception>(reason), reason);
        placed_msg->sender_instance_id = untyped_msg.receiver_instance_id;
        placed_msg->receiver_instance_id = msg.sender_instance_id;
        placed_msg->function_instance_id = msg.function_instance_id;
        placed_msg->exception_type_id = (type_id_type)detail::reserved_id::std_runtime_error;
        s_mproc->m_out_rep_c->done_writing();
    };

    if (!obj) {
        send_unavailable_response("RPC target is no longer available.");
        return;
    }

    auto execution_guard = obj->try_acquire_rpc_execution();
    if (!execution_guard) {
        send_unavailable_response("RPC target is shutting down.");
        return;
    }
    using return_message_type = Message<Enclosure<r_type>, void, not_defined_type_id>;
    static auto once = return_message_type::id();
    (void)(once); // suppress unused variable warning

    type_id_type etid = not_defined_type_id;
    string what;

    auto to_exception_string = [](const char* e_what) -> string
    {
        return e_what ? string(e_what) : string();
    };

    auto vf = Void_filter<r_type>{
        [&](){return call_function_with_message_args(*obj, RPCTC::mf(), msg);}
    };

    clear_rpc_reply_deferred();

    try {
        vf.call();
    }
    catch(std::invalid_argument  &e) { etid = (type_id_type)detail::reserved_id::std_invalid_argument; what = to_exception_string(e.what()); }
    catch(std::domain_error      &e) { etid = (type_id_type)detail::reserved_id::std_domain_error;     what = to_exception_string(e.what()); }
    catch(std::length_error      &e) { etid = (type_id_type)detail::reserved_id::std_length_error;     what = to_exception_string(e.what()); }
    catch(std::out_of_range      &e) { etid = (type_id_type)detail::reserved_id::std_out_of_range;     what = to_exception_string(e.what()); }
    catch(std::range_error       &e) { etid = (type_id_type)detail::reserved_id::std_range_error;      what = to_exception_string(e.what()); }
    catch(std::overflow_error    &e) { etid = (type_id_type)detail::reserved_id::std_overflow_error;   what = to_exception_string(e.what()); }
    catch(std::underflow_error   &e) { etid = (type_id_type)detail::reserved_id::std_underflow_error;  what = to_exception_string(e.what()); }
    catch(std::ios_base::failure &e) { etid = (type_id_type)detail::reserved_id::std_ios_base_failure; what = to_exception_string(e.what()); }
    catch(std::logic_error       &e) { etid = (type_id_type)detail::reserved_id::std_logic_error;      what = to_exception_string(e.what()); }
    catch(std::runtime_error     &e) { etid = (type_id_type)detail::reserved_id::std_runtime_error;    what = to_exception_string(e.what()); }
    catch(std::exception         &e) { etid = (type_id_type)detail::reserved_id::std_exception;        what = to_exception_string(e.what()); }
    catch(...)                       { etid = (type_id_type)detail::reserved_id::unknown_exception;    what = "An exception was thrown whose type is not serialized by sintra";
    }

    if (etid == not_defined_type_id) { // normal return

        if (rpc_reply_is_deferred()) {
            clear_rpc_reply_deferred();
            return;
        }

        // Check if this is a fire-and-forget message (no reply needed)
        if constexpr (RPCTC::is_fire_and_forget) {
            return; // Skip reply for fire-and-forget functions
        }

        // additional return recipients, assumed to be waiting
        if (s_tl_common_function_iid != invalid_instance_id) {

            // NOTE: For the recipients in this loop, this will be the second time the function returns,
            // assuming they have already received a deferral.
            #ifdef SINTRA_BARRIER_DEBUG
            std::fprintf(stderr, "[BARRIER_DEBUG] RPC return: sending %zu additional completions (common_fiid=%llu)\n",
                (size_t)s_tl_additional_piids_size, (unsigned long long)s_tl_common_function_iid);
            std::fflush(stderr);
            #endif
            for (size_t i = 0; i < s_tl_additional_piids_size; i++) {
                #ifdef SINTRA_BARRIER_DEBUG
                std::fprintf(stderr, "[BARRIER_DEBUG]   -> writing completion to %llu via m_out_rep_c=%p\n",
                    (unsigned long long)s_tl_additional_piids[i], (void*)s_mproc->m_out_rep_c);
                std::fflush(stderr);
                #endif
                return_message_type* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(vb_size<return_message_type>(vf.result), vf.result);
                finalize_rpc_write(placed_msg, s_tl_additional_piids[i], s_tl_common_function_iid, obj, not_defined_type_id);
            }

            s_tl_additional_piids_size = 0;
            s_tl_common_function_iid = invalid_instance_id;
        }

        // the normal return
        return_message_type* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(vb_size<return_message_type>(vf.result), vf.result);
        finalize_rpc_write(placed_msg, msg, obj, etid);
    }
    else {
        exception* placed_msg = s_mproc->m_out_rep_c->write<exception>(vb_size<exception>(what), what);
        finalize_rpc_write(placed_msg, msg, obj, etid);
    }
}



template <
    typename RPCTC,
    typename RT,
    typename OBJECT_T,
    typename... FArgs,      // The argument types of the exported member function
    typename... RArgs       // The argument types used by the caller
>
RT Transceiver::rpc(
    RT(OBJECT_T::* /*resolution dummy arg*/)(FArgs...),
    instance_id_type instance_id,
    RArgs&&... args)
{
    using message_type = Message<unique_message_body<RPCTC, FArgs...>, RT, RPCTC::id>;
    return rpc_impl<RPCTC, message_type, FArgs...>(instance_id, args...);
}



template <
    typename RPCTC,
    typename RT,
    typename OBJECT_T,
    typename... FArgs,
    typename... RArgs
>
RT Transceiver::rpc(
    RT(OBJECT_T::* /*resolution_dummy arg*/)(FArgs...) const,
    instance_id_type instance_id,
    RArgs&&... args)
{
    using message_type = Message<unique_message_body<RPCTC, FArgs...>, RT, RPCTC::id>;
    return rpc_impl<RPCTC, message_type, FArgs...>(instance_id, args...);
}



template <
    typename RPCTC,
    typename MESSAGE_T,
    typename... Args
>
typename RPCTC::r_type
Transceiver::rpc_impl(instance_id_type instance_id, Args... args)
{
    if (instance_id == invalid_instance_id) {
        throw std::runtime_error("Attempted to make an RPC call using an invalid instance ID.");
    }

    if (RPCTC::may_be_called_directly && is_local_instance(instance_id)) {
        // if the instance is local, then it has already been registered in the instance_map
        // of this particular type. this will only find the object and call it.
        // Hold spinlock while accessing the iterator to prevent use-after-invalidation
        typename RPCTC::o_type* object = nullptr;
        {
            auto scoped_map = get_instance_to_object_map<RPCTC>().scoped();
            auto it = scoped_map.get().find(instance_id);
            if (it == scoped_map.get().end()) {
                throw std::runtime_error("Local RPC target no longer available - it may have been shut down.");
            }
            object = it->second;
        }
        auto guard = object->try_acquire_rpc_execution();
        if (!guard) {
            throw std::runtime_error("Attempted to call an RPC on a target that is shutting down.");
        }
        return (object->*RPCTC::mf())(args...);
    }

    using return_type = typename MESSAGE_T::return_type;
    using return_message_type = Message<Enclosure<return_type>, void, not_defined_type_id>;

    Outstanding_rpc_control orpcc;
    orpcc.remote_instance = instance_id;
    Unserialized_Enclosure<return_type> rm_body;
    
    type_id_type ex_tid = not_defined_type_id;
    std::string ex_what;

    instance_id_type function_instance_id = invalid_instance_id;

    Return_handler rh;
    rh.return_handler = [&] (const Message_prefix& msg) {
        const auto& returned_message = (const return_message_type&)(msg);
        lock_guard<mutex> sl(orpcc.keep_waiting_mutex);
        rm_body = returned_message;
        orpcc.success = true;
        orpcc.keep_waiting = false;
        orpcc.keep_waiting_condition.notify_all();
    };
    rh.exception_handler = [&] (const Message_prefix& msg) {
        const auto& returned_message = (const exception&)(msg);
        lock_guard<mutex> sl(orpcc.keep_waiting_mutex);
        orpcc.success = false;
        ex_tid = returned_message.exception_type_id;
        ex_what = returned_message.what;
        orpcc.keep_waiting = false;
        orpcc.keep_waiting_condition.notify_all();
    };
    rh.deferral_handler = [&] (const Message_prefix& msg) {

        const auto& returned_message = (const deferral&)(msg);
        lock_guard<mutex> sl(orpcc.keep_waiting_mutex);
        // the 'success' variable here is irrelevant

        // if the new_id differs, then replace it
        assert(returned_message.new_fiid != function_instance_id);
        // Move the active return handler from the temporary id to the final id
        s_mproc->replace_return_handler_id(function_instance_id, returned_message.new_fiid);
        // Keep the caller-side handle in sync so that later cleanup removes the correct entry
        function_instance_id = returned_message.new_fiid;

        // replaces the placement of the handler (message instance id)
        // with the one received by the remote call
        // and keeps waiting until the final result arrives,
        // which will be identified with the replaced message instance id
    };


    // Register the RPC in the outstanding set BEFORE locking keep_waiting_mutex
    // to maintain consistent lock ordering with unblock_rpc (which locks
    // s_outstanding_rpcs_mutex first, then keep_waiting_mutex).
    {
        unique_lock<mutex> orpclock(s_outstanding_rpcs_mutex());
        s_outstanding_rpcs().insert(&orpcc);
    }

    // block until reading thread either receives results or the call fails
    unique_lock<mutex> sl(orpcc.keep_waiting_mutex);

    rh.instance_id = instance_id;
    function_instance_id = s_mproc->activate_return_handler(rh);

    // write the message for the rpc call into the communication ring
    static auto once = MESSAGE_T::id();
    (void)(once); // suppress unused variable warning
    MESSAGE_T* msg = s_mproc->m_out_req_c->write<MESSAGE_T>(vb_size<MESSAGE_T>(args...), args...);
    msg->sender_instance_id = s_mproc->m_instance_id;
    msg->receiver_instance_id = instance_id;
    msg->function_instance_id = function_instance_id;
    s_mproc->m_out_req_c->done_writing();

    //    _       .//'
    //   (_).  .//'                          TODO: for an asynchronous implementation, cut here
    // -- _  O|| --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --
    //   (_)'  '\\.
    //            '\\.

    // Check if this is a fire-and-forget message (no reply expected)
    if constexpr (RPCTC::is_fire_and_forget) {
        // For fire-and-forget functions, don't wait for a reply
        sl.unlock();
        {
            unique_lock<mutex> orpclock(s_outstanding_rpcs_mutex());
            s_outstanding_rpcs().erase(&orpcc);
        }
        s_mproc->deactivate_return_handler(function_instance_id);
        // Return void (void_placeholder_t will be handled by the caller)
        return rm_body.get_value();
    }

    // Wait until either a normal reply arrives or the RPC gets unblocked.
    // With predicate waiting plus Managed_process::unblock_rpc() (invoked on
    // coordinator loss and during finalize()), this wait is bounded and cannot
    // deadlock via lost notifications.
    orpcc.keep_waiting_condition.wait(sl, [&]{ return !orpcc.keep_waiting; });

    // Release per-RPC mutex before touching the global outstanding set.
    // This maintains the lock ordering rule: always acquire s_outstanding_rpcs_mutex
    // before any keep_waiting_mutex (never the reverse).
    sl.unlock();

    {
        unique_lock<mutex> orpclock(s_outstanding_rpcs_mutex());
        s_outstanding_rpcs().erase(&orpcc);
    }

    // we can now disable the return message handler
    s_mproc->deactivate_return_handler(function_instance_id);

    if (!orpcc.success) {
        if (ex_tid != not_defined_type_id) {
            // interprocess exception
            string_to_exception(ex_tid, ex_what);
        }
        else {
            // rpc failure (distinguish between cancelled vs. other failures)
            if (orpcc.cancelled) {
                throw rpc_cancelled("rpc cancelled");
            }
            else {
                throw std::runtime_error("RPC failed");
            }
        }
    }

    return rm_body.get_value();
}



template<typename>
instance_id_type
Transceiver::activate_return_handler(const Return_handler &rh)
{
    instance_id_type function_instance_id = make_instance_id();
    lock_guard<mutex> sl(m_return_handlers_mutex);
    m_active_return_handlers[function_instance_id] = rh;
    return function_instance_id;
}



inline
void
Transceiver::deactivate_return_handler(instance_id_type function_instance_id)
{
    lock_guard<mutex> sl(m_return_handlers_mutex);
    m_active_return_handlers.erase(function_instance_id);
}


inline
void
Transceiver::replace_return_handler_id(instance_id_type old_id, instance_id_type new_id)
{
    lock_guard<mutex> sl(m_return_handlers_mutex);
    auto it = m_active_return_handlers.find(old_id);
    if (it == m_active_return_handlers.end()) {
        // The handler may have already been remapped by a previous deferral or
        // unblocked due to coordinator shutdown. In that case there's nothing
        // left to move and the existing state already reflects the most recent
        // function id.
        return;
    }

    auto handler = std::move(it->second);
    m_active_return_handlers.erase(it);
    m_active_return_handlers[new_id] = std::move(handler);
}



template <typename RPCTC, typename MT>
function<void()>
Transceiver::export_rpc_impl()
{
    warn_about_reference_return<typename RPCTC::r_type>();
    warn_about_reference_args<MT>();

    auto* self = static_cast<typename RPCTC::o_type*>(this);
    const auto instance_id = m_instance_id;

    get_instance_to_object_map<RPCTC>()[instance_id] = self;

    uint64_t test = MT::id();

    // handler registration
    using RPCTC_o_type = typename RPCTC::o_type;
    static auto once = get_rpc_handler_map()[test] =
        &RPCTC_o_type::template rpc_handler<RPCTC, MT>;
    (void)(once); // suppress unused variable warning

    return [instance_id]() {
        // Note: do NOT call ensure_rpc_shutdown() here - the transceiver may already
        // be destroyed by the time this cleanup lambda is invoked during finalize().
        // RPC shutdown is handled by the transceiver's destructor (via destroy()).
        get_instance_to_object_map<RPCTC>().erase(instance_id);
    };
}



template <typename RPCTC, typename RT, typename OBJECT_T, typename... Args>
function<void()>
Transceiver::export_rpc(RT(OBJECT_T::* /*resolution dummy arg*/)(Args...) const)
{
    using message_type = Message<unique_message_body<RPCTC, Args...>, RT, RPCTC::id>;
    return export_rpc_impl<RPCTC, message_type>();
}



template <typename RPCTC, typename RT, typename OBJECT_T, typename... Args>
function<void()>
Transceiver::export_rpc(RT(OBJECT_T::* /*resolution dummy arg*/)(Args...))
{
    using message_type = Message<unique_message_body<RPCTC, Args...>, RT, RPCTC::id>;
    return export_rpc_impl<RPCTC, message_type>();
}



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END RPC //////////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////



} // namespace sintra


