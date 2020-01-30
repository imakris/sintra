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

#ifndef SINTRA_TRANSCEIVER_IMPL_H
#define SINTRA_TRANSCEIVER_IMPL_H


#include <type_traits>
#include <memory>


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

    activate(
        &Transceiver::instance_invalidated_handler,
        Typed_instance_id<void>(any_local_or_remote) );
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
    if (!s_mproc || !s_coord_id || s_mproc->m_readers.empty()) {
        // If the process is not running, there is nothing to facilitate the basic
        // functionality of a Transceiver object. This can happen if the process object is
        // itself being destroyed.
        return;
    }

    if (this != s_mproc) {
        deactivate_all();
    }

    if (m_published) {

        // If the message ring threads are not running, attempting rpc would deadlock,
        // which must be prevented. This is a likely scenario on an emergency exit.
        if (s_mproc->m_state >= Managed_process::PAUSED) {
            auto success = Coordinator::rpc_unpublish_transceiver(s_coord_id, m_instance_id);
            // TODO: FIXME: there is still no implementation to handle failure
            assert(success);
        }

        m_published = false;

        // if the coordinator is local, it would be deleted already in the unpublish call
        if (!s_coord) {
            s_mproc->m_instance_id_of_assigned_name.erase(m_cache_iterator);
        }
    }
    else
    if (m_instance_id != s_coord_id) {
        s_mproc->m_local_pointer_of_instance_id.erase(m_instance_id);
    }
}



inline
void Transceiver::instance_invalidated_handler(const instance_invalidated& msg)
{
    lock_guard<mutex> sl(m_return_handlers_mutex);

    auto it = m_active_return_handlers.begin();
    while (it != m_active_return_handlers.end()) {
        if (it->second.instance_id == msg.sender_instance_id) {
            // if this transceiver is waiting on an rpc call to a function
            // of the transceiver being invalidated, the call will fail.
            it->second.failure_handler();
            it = m_active_return_handlers.erase(it);
        }
        else {
            ++it;
        }
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

    lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);

    auto& ms  = s_mproc->m_active_handlers[message_type_id];
    list<function<void(const Message_prefix &)>>::iterator mid_sid_it;

    auto  msm_it = ms.find(sender_id);
    if (msm_it == ms.end()) {

        // There was no record for this sender_id, thus we have to make one.

        msm_it = ms.emplace(
            sender_id,
            list<function<void(const Message_prefix&)>> {
                (function<void(const Message_prefix&)>&) handler
            }
        ).first;
        mid_sid_it = msm_it->second.begin();
    }
    else {
        mid_sid_it = msm_it->second.emplace(msm_it->second.end(),
            (function<void(const Message_prefix&)>&) handler
        );
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
    auto handler =
        function<typename MESSAGE_T::return_type(const MESSAGE_T&)>(
            boost::bind(v, static_cast<OBJECT_T*>(this), _1));

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

    MESSAGE_T* msg = s_mproc->m_out_req_c->write<MESSAGE_T>(vb_size(args...), args...);
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
    R_MESSAGE_T* placed_msg, const MESSAGE_T& msg, const OBJECT_T* obj)
{
    placed_msg->sender_instance_id = obj->m_instance_id;
    placed_msg->receiver_instance_id = msg.sender_instance_id;
    placed_msg->function_instance_id = msg.function_instance_id;
    placed_msg->message_type_id = not_defined_type_id;
    s_mproc->m_out_rep_c->done_writing();
}



template <
    typename RPCTC,
    typename MESSAGE_T,
    typename /* = void*/,
    typename /* = enable_if_t<is_same< typename RPCTC::r_type, void>::value> */
>
void Transceiver::rpc_handler(Message_prefix& untyped_msg)
{
    MESSAGE_T& msg = (MESSAGE_T&)untyped_msg;
    typename RPCTC::o_type* obj = get_instance_to_object_map<RPCTC>()[untyped_msg.receiver_instance_id];
    using return_message_type = Message<Enclosure<typename RPCTC::r_type>, void, not_defined_type_id>;
    static auto once = return_message_type::id();
    (void)(once); // suppress unused variable warning
    call_function_with_fusion_vector_args(*obj, RPCTC::mf(), msg);
    return_message_type* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(0);
    finalize_rpc_write(placed_msg, msg, obj);
}



template <
    typename RPCTC,
    typename MESSAGE_T,
    typename /* = enable_if_t<!is_same< typename RPCTC::r_type, void>::value> */
>
void Transceiver::rpc_handler(Message_prefix& untyped_msg)
{
    MESSAGE_T& msg = (MESSAGE_T&)untyped_msg;
    typename RPCTC::o_type* obj = get_instance_to_object_map<RPCTC>()[untyped_msg.receiver_instance_id];
    using return_message_type = Message<Enclosure<typename RPCTC::r_type>, void, not_defined_type_id>;
    static auto once = return_message_type::id();
    (void)(once); // suppress unused variable warning
    typename RPCTC::r_type ret = call_function_with_fusion_vector_args(*obj, RPCTC::mf(), msg);
    return_message_type* placed_msg =
        s_mproc->m_out_rep_c->write<return_message_type>(vb_size(ret), ret);
    finalize_rpc_write(placed_msg, msg, obj);
}



template <
    typename RPCTC,
    typename RT,
    typename OBJECT_T,
    typename... FArgs,      // The argument types of the exported member function
    typename... RArgs       // The artument types used by the caller
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
    if (is_local_instance(instance_id)) {
        // if the instance is local, then it has already been registered in the instance_map
        // of this particular type. this will only find the object and call it.
        auto it = get_instance_to_object_map<RPCTC>().find(instance_id);
        assert(it != get_instance_to_object_map<RPCTC>().end());
        return (it->second->*RPCTC::mf())(args...);
    }    

    using return_type = typename MESSAGE_T::return_type;
    using return_message_type = Message<Enclosure<return_type>, void, not_defined_type_id>;

    mutex               keep_waiting_mutex;
    condition_variable  keep_waiting_condition;
    bool                keep_waiting = true;
    bool                success = false;

    Unserialized_Enclosure<return_type> rm_body;
    Return_handler rh;
    rh.success_handler = [&] (const Message_prefix& msg) {
        const auto& returned_message = (const return_message_type&)(msg);
        lock_guard<mutex> sl(keep_waiting_mutex);
        rm_body = returned_message;
        success = true;
        keep_waiting = false;
        keep_waiting_condition.notify_all();
    };
    rh.failure_handler = [&] () {
        lock_guard<mutex> sl(keep_waiting_mutex);
        success = false;
        keep_waiting = false;
        keep_waiting_condition.notify_all();
    };
    rh.instance_id = instance_id;


    auto function_instance_id = s_mproc->activate_return_handler(rh);

    // block until reading thread either receives results or the call fails
    unique_lock<mutex> sl(keep_waiting_mutex);

    // write the message for the rpc call into the communication ring
    static auto once = MESSAGE_T::id();
    (void)(once); // suppress unused variable warning
    MESSAGE_T* msg = s_mproc->m_out_req_c->write<MESSAGE_T>(vb_size(args...), args...);
    msg->sender_instance_id = s_mproc->m_instance_id;
    msg->receiver_instance_id = instance_id;
    msg->function_instance_id = function_instance_id;
    s_mproc->m_out_req_c->done_writing();

    //    _       .//'
    //   (_).  .//'                          TODO: for an asynchronous implementation, cut here
    // -- _  ::: --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --  --
    //   (_)'  '\\.
    //            '\\.

    while (keep_waiting) {
        // TODO: FIXME
        // If one of the processes supplying results crashes without notice or blocks,
        // this will deadlock. We need to put a time limit, implement recovery and test it.
        //  use wait_for, rather than wait - but not just yet -
        keep_waiting_condition.wait(sl);
    }
    sl.unlock();

    // we can now disable the return message handler
    s_mproc->deactivate_return_handler(function_instance_id);

    if (!success) {
        // This should be unreachable, until an abort mechanism is implemented.
        throw runtime_error("RPC failed");
    }

    return rm_body.get_value();
}



inline
instance_id_type
Transceiver::activate_return_handler(const Return_handler &rh)
{
    instance_id_type message_instance_id = make_instance_id();
    lock_guard<mutex> sl(m_return_handlers_mutex);
    m_active_return_handlers[message_instance_id] = rh;
    return message_instance_id;
}



inline
void
Transceiver::deactivate_return_handler(instance_id_type message_instance_id)
{
    lock_guard<mutex> sl(m_return_handlers_mutex);
    m_active_return_handlers.erase(message_instance_id);
}



template <typename RPCTC, typename MT>
function<void()>
Transceiver::export_rpc_impl()
{
    warn_about_reference_return<typename RPCTC::r_type>();
    warn_about_reference_args<MT>();

    get_instance_to_object_map<RPCTC>()[m_instance_id] = static_cast<typename RPCTC::o_type*>(this);

    uint64_t test = MT::id();

    // handler registration
    using RPCTC_o_type = typename RPCTC::o_type;
    static auto once = get_rpc_handler_map()[test] =
        &RPCTC_o_type::template rpc_handler<RPCTC, MT>;
    (void)(once); // suppress unused variable warning

    return [&] () {get_instance_to_object_map<RPCTC>().erase(m_instance_id); };
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


#endif
