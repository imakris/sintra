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

#ifndef __SINTRA_TRANSCEIVER_IMPL_H__
#define __SINTRA_TRANSCEIVER_IMPL_H__


namespace sintra {


using std::enable_if;
using std::function;
using std::make_pair;
using std::remove_reference;
using std::runtime_error;
using std::string;



template <typename/* = void*/>
Transceiver::Transceiver(const string& name/* = ""*/, uint64_t id/* = 0*/)
{
    m_instance_id = id ? id : make_instance_id();

    if (m_instance_id == invalid_instance_id) {
        throw runtime_error("Transceiver instance allocation failed");
    }

    mproc::s->m_local_pointer_of_instance_id[m_instance_id] = this;

    // A transceiver instance may as well not have a name (in which case, name lookups fail).
    if (!name.empty()) {
        if (!assign_name(name)) {
            throw runtime_error("Transceiver instance allocation failed");
        }
    }

    activate(&Transceiver::instance_invalidated_handler, any_local_or_remote);
}



inline
Transceiver::~Transceiver()
{
    destroy();
}



template <typename/* = void*/>
bool Transceiver::assign_name(const string& name)
{
    if (Coordinator::rpc_publish_transceiver(coord_id::s, m_instance_id, name)) {
        m_named = true;

        if (!coord::s) {
            auto cache_entry = make_pair(name, m_instance_id);
            auto rvp = mproc::s->m_instance_id_of_name.insert(cache_entry);
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
    if (!mproc::s || !coord_id::s || mproc::s->m_readers.empty()) {
        // If the process is not running, there is nothing to facilitate the basic
        // functionality of a Transceiver object. This can happen if the process object is
        // itself being destroyed.
        return;
    }

    deactivate_all();

    if (m_named) {
        auto success = Coordinator::rpc_unpublish_transceiver(coord_id::s, m_instance_id);
        assert(success);

        // if the coordinator is local, it would be deleted already in the unpublish call
        if (!coord::s) {
            mproc::s->m_instance_id_of_name.erase(m_cache_iterator);
        }
    }
    else
    if (m_instance_id != coord_id::s) {
        mproc::s->m_local_pointer_of_instance_id.erase(m_instance_id);
    }
}



inline
void Transceiver::instance_invalidated_handler(const instance_invalidated& msg)
{
    lock_guard<mutex> sl(m_return_handlers_mutex);

    auto it = m_active_return_handlers.begin();
    while (it != m_active_return_handlers.end()) {
        if (it->second.instance_id == msg.instance_id) {
            it->second.failure_handler();
            it = m_active_return_handlers.erase(it);
        }
        else {
            ++it;
        }
    }
}



template<typename MESSAGE_TYPE, typename HT>
Transceiver::handler_provoker_desrcriptor
Transceiver::activate_impl(HT&& handler, instance_id_type sender_id)
{
    auto message_type_id = MESSAGE_TYPE::id();
    lock_guard<mutex> sl(m_handlers_mutex);

    auto f = (function<void(const Message_prefix&)>&) handler;
    auto mid_proc_iterator = mproc::s->m_active_handlers[message_type_id].emplace(
        make_pair(sender_id, f));

    auto& ahir = m_active_handler_iterators[message_type_id];
    auto mid_self_iterator = ahir.emplace(ahir.end(), mid_proc_iterator);

    return handler_provoker_desrcriptor{message_type_id, mid_self_iterator};
}



// A functor with an arbitrary non-message argument
template<
    typename FT,
    typename  /* = decltype(&FT::operator())*/,  //must be functor
    typename FUNCTOR_ARG_TYPE /* = decltype(resolve_single_functor_arg(*((FT*)0)))*/,

    // prevent functors with message arguments from matching the template
    typename /* = typename enable_if_t<
        !std::is_base_of<
            Message_prefix,
            typename std::remove_reference<FUNCTOR_ARG_TYPE>::type
        >::value
    >*/
>
typename Transceiver::handler_provoker_desrcriptor
Transceiver::activate(const FT& internal_slot, instance_id_type sender_id)
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

    return activate_impl<MT>(handler, sender_id);
}



// A functor with a message argument
template<
    typename FT,
    typename  /* = decltype(&FT::operator())*/,    //must be functor
    typename  /* = void*/, // differentiate from the previous template
    typename FUNCTOR_ARG_TYPE /* = decltype(resolve_single_functor_arg(*((FT*)0)))*/,

    // only allow functors with message arguments to match the template
    typename /* = typename enable_if_t<
        is_base_of<
            Message_prefix,
            typename remove_reference<FUNCTOR_ARG_TYPE>::type
        >::value
    >*/
>
typename Transceiver::handler_provoker_desrcriptor
Transceiver::activate(const FT& internal_slot, instance_id_type sender_id)
{
    // the given slot is already a functor taking a message argument, thus there is no need for
    // inclusion into a lambda. There is however the need to convert to function, since
    // we don't know what kind of functor it is.

    using arg_type = decltype(resolve_single_functor_arg(internal_slot));
    using MT = typename remove_reference<arg_type>::type;
    auto handler = static_cast<function<typename MT::return_type(const arg_type&)>>(internal_slot);

    return activate_impl<MT>(handler, sender_id);
}



// A Transceiver member function with a message argument. The sender has to exist.
template<
    typename MESSAGE_TYPE,
    typename OBJECT_TYPE,
    typename RT /* = typename MESSAGE_TYPE::return_type*/
>
Transceiver::handler_provoker_desrcriptor
Transceiver::activate(
    RT(OBJECT_TYPE::*v)(const MESSAGE_TYPE&), 
    instance_id_type sender_id)
{
    auto handler =
        function<typename MESSAGE_TYPE::return_type(const MESSAGE_TYPE&)>(
            boost::bind(v, static_cast<OBJECT_TYPE*>(this), _1));

    return activate_impl<MESSAGE_TYPE>(handler, sender_id);
}



template <typename /* = void*/>
void Transceiver::deactivate(handler_provoker_desrcriptor pd)
{
    auto& pah = mproc::s->m_active_handlers;
    auto& ahi = m_active_handler_iterators;

    const auto& hr_proc = pah.find(pd.message_type_id);
    const auto& hr_self = ahi.find(pd.message_type_id);
        
    assert(hr_proc != pah.end());
    assert(hr_self != ahi.end());

    hr_proc->second.erase(*pd.mid_self_iterator);
    hr_self->second.erase( pd.mid_self_iterator);

    if (!hr_proc->second.size()) {
        pah.erase(hr_proc);
    }
    if (!hr_self->second.size()) {
        ahi.erase(hr_self);
    }
}



template <typename /* = void*/>
void Transceiver::deactivate_all()
{
    for (auto& el1 : m_active_handler_iterators) {
        
        for (auto& el2 : el1.second) {
            mproc::s->m_active_handlers[el1.first].erase(el2);
        }
    }

    m_active_handler_iterators.clear();
}



template <
    typename MESSAGE_TYPE,
    instance_id_type LOCALITY,
    typename /* = void*/,
    typename... Args>
void Transceiver::send(Args&&... args)
{
    static auto once = MESSAGE_TYPE::id();

    MESSAGE_TYPE* msg = mproc::s->m_out_req_c->write<MESSAGE_TYPE>(vb_size(args...), args...);
    msg->sender_instance_id = m_instance_id;
    msg->receiver_instance_id = LOCALITY;
    mproc::s->m_out_req_c->done_writing();
}


 //////////////////////////////////////////////////////////////////////////
///// BEGIN RPC ////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////    //////    //////    //////    //////    //////    //////    //////
 ////      ////      ////      ////      ////      ////      ////      ////
  //        //        //        //        //        //        //        //



// This is only for RPC, thus static. Since the handler maps to a class/struct function, all
// instances of this class/struct use the same handler, which is just a function enclosing
// a __thiscall to whichever member function was exported.
inline
auto& Transceiver::get_rpc_handler_map()
{
    static spinlocked_map<type_id_type, void(*)(Message_prefix&)> message_id_to_handler;
    return message_id_to_handler;
}



template <typename RPCTC>
auto& Transceiver::get_instance_to_object_map()
{
    static spinlocked_map<instance_id_type, typename RPCTC::o_type*> instance_to_object;
    return instance_to_object;
}



template <typename R_MESSAGE_TYPE, typename MESSAGE_TYPE, typename OBJ_TYPE>
void Transceiver::finalize_rpc_write(
    R_MESSAGE_TYPE* placed_msg, const MESSAGE_TYPE& msg, const OBJ_TYPE* obj)
{
    placed_msg->sender_instance_id = obj->m_instance_id;
    placed_msg->receiver_instance_id = msg.sender_instance_id;
    placed_msg->function_instance_id = msg.function_instance_id;
    placed_msg->message_type_id = not_defined_type_id;
    mproc::s->m_out_rep_c->done_writing();
}



template <
    typename RPCTC,
    typename MESSAGE_TYPE,
    typename /* = void*/,
    typename /* = enable_if_t<is_same< typename RPCTC::r_type, void>::value> */
>
void Transceiver::rpc_handler(Message_prefix& untyped_msg)
{
    MESSAGE_TYPE& msg = (MESSAGE_TYPE&)untyped_msg;
    typename RPCTC::o_type* obj = get_instance_to_object_map<RPCTC>()[untyped_msg.receiver_instance_id];
    using return_message_type = Message<Enclosure<typename RPCTC::r_type>, void, not_defined_type_id>;
    static auto once = return_message_type::id();
    call_function_with_fusion_vector_args(*obj, RPCTC::mf(), msg);
    return_message_type* placed_msg = mproc::s->m_out_rep_c->write<return_message_type>(0);
    finalize_rpc_write(placed_msg, msg, obj);
}



template <
    typename RPCTC,
    typename MESSAGE_TYPE,
    typename /* = enable_if_t<!is_same< typename RPCTC::r_type, void>::value> */
>
void Transceiver::rpc_handler(Message_prefix& untyped_msg)
{
    MESSAGE_TYPE& msg = (MESSAGE_TYPE&)untyped_msg;
    typename RPCTC::o_type* obj = get_instance_to_object_map<RPCTC>()[untyped_msg.receiver_instance_id];
    using return_message_type = Message<Enclosure<typename RPCTC::r_type>, void, not_defined_type_id>;
    static auto once = return_message_type::id();
    typename RPCTC::r_type ret = call_function_with_fusion_vector_args(*obj, RPCTC::mf(), msg);
    return_message_type* placed_msg =
        mproc::s->m_out_rep_c->write<return_message_type>(vb_size(ret), ret);
    finalize_rpc_write(placed_msg, msg, obj);
}



template <
    typename RPCTC,
    typename RT,
    typename OBJECT_TYPE,
    typename... FArgs,      // The argument types of the exported member function
    typename... RArgs       // The artument types used by the caller
>
RT Transceiver::rpc(
    RT(OBJECT_TYPE::*resolution_dummy)(FArgs...),
    instance_id_type instance_id,
    RArgs&&... args)
{
    using message_type = Message<unique_message_body<RPCTC, FArgs...>, RT, RPCTC::id>;
    return rpc_impl<RPCTC, message_type, FArgs...>(instance_id, args...);
}



template <
    typename RPCTC,
    typename RT,
    typename OBJECT_TYPE,
    typename... FArgs,
    typename... RArgs
>
RT Transceiver::rpc(
    RT(OBJECT_TYPE::*resolution_dummy)(FArgs...) const,
    instance_id_type instance_id,
    RArgs&&... args)
{
    using message_type = Message<unique_message_body<RPCTC, FArgs...>, RT, RPCTC::id>;
    return rpc_impl<RPCTC, message_type, FArgs...>(instance_id, args...);
}



template <
    typename RPCTC,
    typename MESSAGE_TYPE,
    typename... Args
>
typename RPCTC::r_type
Transceiver::rpc_impl(instance_id_type instance_id, Args... args)
{
    if (is_local_instance(instance_id))
    {
        // if the instance is local, then it has already been registered in the instance_map
        // of this particular type. this will only find the object and call it.
        auto it = get_instance_to_object_map<RPCTC>().find(instance_id);
        assert(it != get_instance_to_object_map<RPCTC>().end());
        return (it->second->*RPCTC::mf())(args...);
    }

    using return_type = typename MESSAGE_TYPE::return_type;
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


    auto function_instance_id = mproc::s->activate_return_handler(rh);

    // block until reading thread either receives results or the call fails
    unique_lock<mutex> sl(keep_waiting_mutex);

    // write the message for the rpc call into the communication ring
    static auto once = MESSAGE_TYPE::id();
    MESSAGE_TYPE* msg = mproc::s->m_out_req_c->write<MESSAGE_TYPE>(vb_size(args...), args...);
    msg->sender_instance_id = mproc::s->m_instance_id;
    msg->receiver_instance_id = instance_id;
    msg->function_instance_id = function_instance_id;
    mproc::s->m_out_req_c->done_writing();

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
    mproc::s->deactivate_return_handler(function_instance_id);

    if (!success) {
        // unlil an abort mechanism is implemented, this is unreachable.
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

    return [&] () {get_instance_to_object_map<RPCTC>().erase(m_instance_id); };
}



template <typename RPCTC, typename RT, typename OBJECT_TYPE, typename... Args>
function<void()>
Transceiver::export_rpc(RT(OBJECT_TYPE::*resolution_dummy)(Args...) const)
{
    using message_type = Message<unique_message_body<RPCTC, Args...>, RT, RPCTC::id>;
    return export_rpc_impl<RPCTC, message_type>();
}



template <typename RPCTC, typename RT, typename OBJECT_TYPE, typename... Args>
function<void()>
Transceiver::export_rpc(RT(OBJECT_TYPE::*resolution_dummy)(Args...))
{
    using message_type = Message<unique_message_body<RPCTC, Args...>, RT, RPCTC::id>;
    return export_rpc_impl<RPCTC, message_type>();
}



  //        //        //        //        //        //        //        //
 ////      ////      ////      ////      ////      ////      ////      ////
//////    //////    //////    //////    //////    //////    //////    //////
////////////////////////////////////////////////////////////////////////////
///// END RPC //////////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////



} // namespace sintra


#endif
