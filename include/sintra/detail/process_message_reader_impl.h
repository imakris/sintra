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

#ifndef SINTRA_PROCESS_MESSAGE_READER_IMPL_H
#define SINTRA_PROCESS_MESSAGE_READER_IMPL_H


#include "transceiver_impl.h"


namespace sintra {


using std::thread;


inline
Process_message_reader::Process_message_reader(instance_id_type process_instance_id):
    m_status(FULL_FUNCTIONALITY),
    m_process_instance_id(process_instance_id)
{
    if (is_local_instance(m_process_instance_id)) {
        m_in_req_c = new Message_ring_R(s_mproc->m_directory, "req", m_process_instance_id);
        m_request_reader_thread = new thread([&] () { local_request_reader_function(); });
    }
    else {
        m_in_req_c = new Message_ring_R(s_mproc->m_directory, "req", m_process_instance_id);
        m_in_rep_c = new Message_ring_R(s_mproc->m_directory, "rep", m_process_instance_id);
        m_request_reader_thread = new thread([&] () { request_reader_function(); });
        m_reply_reader_thread   = new thread([&] () { reply_reader_function();   });
    }
}


inline
void Process_message_reader::wait_until_ready()
{
    while (!m_in_req_c->get_sleeping_readers()) {}

    if (!is_local_instance(m_process_instance_id)) {
        while (!m_in_rep_c->get_sleeping_readers()) {}
    }
}


inline
Process_message_reader::~Process_message_reader()
{
    using namespace std::chrono;

    if (is_local_instance(m_process_instance_id)) {
        std::unique_lock<std::mutex> lk(m_req_stop_mutex);
        stop();
        m_in_req_c->unblock();
        auto req_predicate = [&] {return m_req_running == false;};
        m_req_stop_condition.wait_for(lk, duration<double>(2.), req_predicate);

        if (m_req_running == false) {
            m_request_reader_thread->join();
        }
        else {
            // wait_for timed out. To avoid hanging, on an attempt to join, we exit.
            exit(1);
        }

        delete m_request_reader_thread;
        delete m_in_req_c;
    }
    else {
        std::unique_lock<std::mutex> lk1(m_req_stop_mutex);
        std::unique_lock<std::mutex> lk2(m_rep_stop_mutex);
        stop();
        m_in_req_c->unblock();
        m_in_rep_c->unblock();
        auto req_predicate = [&]{return m_req_running == false;};
        m_req_stop_condition.wait_for(lk1, duration<double>(2.), req_predicate);
        auto rep_predicate = [&]{return m_rep_running == false;};
        m_rep_stop_condition.wait_for(lk2, duration<double>(2.), rep_predicate);

        if (m_req_running == false) {
            m_request_reader_thread->join();
        }
        else {
            // wait_for timed out. To avoid hanging, on an attempt to join, we exit.
            exit(1);
        }

        if (m_rep_running == false) {
            m_reply_reader_thread->join();
        }
        else {
            // wait_for timed out. To avoid hanging, on an attempt to join, we exit.
            exit(1);
        }

        delete m_reply_reader_thread;
        delete m_request_reader_thread;
        delete m_in_rep_c;
        delete m_in_req_c;
    }
}


// This implementation of the following functions assumes the following:
// We have two types of messages: rpc messages and events
// - RPC messages are only addressed to specific receivers. All receivers able to handle this
//   type of message use the same handler, thus the handler map is static.
// - Events are only addressed "to whom it may concern" (i.e. any_*).
//   Their handlers are registered at process level, but they are assigned dynamically, thus
//   they may differ across different instances of the same type of receiver type.


inline
void Process_message_reader::request_reader_function()
{
    m_req_running = true;

    while (m_status != STOPPING) {
        s_tl_current_message = nullptr;
        Message_prefix* m = m_in_req_c->fetch_message();

        s_tl_current_message = m;

        if (m == nullptr) {
            break;
        }

        // Only the process with the coordinator's instance is allowed to send messages on
        // someone else's behalf (for relay purposes).
        // TODO: If some process not being part of the core set of processes sends nonsense,
        // it might be a good idea to kill it. If it is in the core set of processes,
        // then it would be a bug.
        assert(m_in_req_c->m_id == process_of(m->sender_instance_id) ||
               m_in_req_c->m_id == process_of(s_coord_id));

        if (is_local_instance(m->sender_instance_id) && m->receiver_instance_id == any_remote) {

            // This covers the following scenario:
            // - a local process without resident coordinator sends a message to any_remote.
            // - the message is first written to the processe's write channel, which is read by
            //   the coordinator.
            // - then the coordinator's process writes it to its own write channel (relay)
            // - the process that this message originated from, which reads the write channel of
            //   the coordinator's process, will subsequently read a message that is local to
            //   it, even though it is from an external channel, which is a paradox.
            // Such messages must be ignored.

            assert(!s_coord);
            continue;
        }

        assert(m->message_type_id != not_defined_type_id);

        if (is_local_instance(m->receiver_instance_id)) {

            if (m_status == FULL_FUNCTIONALITY) {
                auto it = s_mproc->m_local_pointer_of_instance_id.find(m->receiver_instance_id);

                // If addressed to a specified local receiver, this may only be an RPC call,
                // thus the named receiver must exist.

                if (it == s_mproc->m_local_pointer_of_instance_id.end()) {
                    // that's an exception, and an exception message should be sent back
                    assert(false); // FIXME: IMPLEMENT
                }
                else {
                    // if the receiver has a registered handler, call the handler
                    auto it2 = Transceiver_base::get_rpc_handler_map().find(m->message_type_id);
                    if (it2 == Transceiver_base::get_rpc_handler_map().end()) {
                        assert(false); // same here  // FIXME: IMPLEMENT
                    }
                    else {
                        // just call the handler
                        (*it2->second)(*m);
                    }
                }
            }
            else // COORDINATOR_ONLY
            if ((m->receiver_instance_id == s_coord_id && s_coord) ||
                (m->sender_instance_id == s_coord_id) )
            {
                // If addressed to a specified local receiver (in this case the
                // coordinator), this may only be an RPC call.

                // if the receiver has a registered handler, call the handler
                auto it2 = Transceiver_base::get_rpc_handler_map().find(m->message_type_id);
                if (it2 == Transceiver_base::get_rpc_handler_map().end()) {
                    assert(false); // same here  // FIXME: IMPLEMENT
                }
                else {
                    // just call the handler
                    (*it2->second)(*m);
                }
            }
        }
        else
        if (m->receiver_instance_id >= any_remote) {

            // this is an interprocess event message.

            if (m_status == FULL_FUNCTIONALITY) {
                    
                // [ NEW IMPLEMENTATION - NOT COVERED ]
                // find handlers that operate with this type of message in this process
                auto it_mt = s_mproc->m_active_handlers.find(m->message_type_id);
                if (it_mt != s_mproc->m_active_handlers.end()) {

                    
                    instance_id_type sids[] = {
                        m->sender_instance_id,
                        any_remote,
                        any_local_or_remote
                    };

                    for (auto sid : sids) {
                        auto shl = it_mt->second.find(sid);
                        if (shl != it_mt->second.end()) {
                            for (auto& e : shl->second) {
                                e(*m);
                            }
                        }
                    }

                }
            }

            // if the coordinator is in this process, relay
            if (s_coord) {
                s_mproc->m_out_req_c->relay(*m);
            }
        }
        else {
            // a local event has no place in interprocess messages
            // this would be a bug.
            assert(m->receiver_instance_id != any_local);

            // a specific non-local receiver means an rpc to another process.
            // if the coordinator is in this process, relay
            if (s_coord) {
                // the message type is specified, thus it is a request
                s_mproc->m_out_req_c->relay(*m);
            }
        }
    }

    m_in_req_c->done_reading();

    std::unique_lock<std::mutex> lk(m_req_stop_mutex);
    m_req_running = false;
    m_req_stop_condition.notify_one();
}



inline
void Process_message_reader::local_request_reader_function()
{
    // - There should be no remote rpc requests in this function. Local RPC should never find
    //     its way into any ring.
    // - There should also not be any COORDINATOR_ONLY state.
    // This state exists to facilitate RPC with a remote coordinator.
    // However, if the coordinator is remote, a local reader is irrelevant, and if
    // it is local, ring RPC is also irrelevant.

    m_req_running = true;

    while (m_status != STOPPING) {
        s_tl_current_message = nullptr;
        Message_prefix* m = m_in_req_c->fetch_message();


        s_tl_current_message = m;

        if (m == nullptr) {
            break;
        }

        // Only the process with the coordinator's instance is allowed to send messages on
        // someone else's behalf (for relay purposes).
        // TODO: If some process not being part of the core set of processes sends nonsense,
        // it might be a good idea to kill it. If it is in the core set of processes,
        // then it would be a bug.
        assert(m_in_req_c->m_id == process_of(m->sender_instance_id) ||
               m_in_req_c->m_id == process_of(s_coord_id));

        if (is_local_instance(m->sender_instance_id) && m->receiver_instance_id == any_remote) {

            // This is the message that the local process sent and is being relayed in the
            // coordinator's ring. The Coordinator's ring is only one, and observed by all
            // processes, thus the messages are visible to their sender.
            // But if they are addressed to any_remote, the sender should ignore them.
                    
            assert(!s_coord);
            continue;
        }

        assert(m->message_type_id != not_defined_type_id);

        if (is_local_instance(m->receiver_instance_id)) {

            auto it = s_mproc->m_local_pointer_of_instance_id.find(m->receiver_instance_id);

            // If addressed to a specified local receiver, this may only be an RPC call,
            //thus the named receiver must exist.

            if (it == s_mproc->m_local_pointer_of_instance_id.end()) {
                // that's an exception, and an exception message should be sent back
                assert(false); // FIXME: IMPLEMENT
            }
            else {
                // if the receiver has a registered handler, call the handler
                auto it2 = Transceiver_base::get_rpc_handler_map().find(m->message_type_id);
                if (it2 == Transceiver_base::get_rpc_handler_map().end()) {
                    assert(false); // same here  // FIXME: IMPLEMENT
                }
                else {
                    // just call the handler
                    (*it2->second)(*m);
                }
            }
        }
        else
        if (m->receiver_instance_id == any_local ||
            m->receiver_instance_id == any_local_or_remote)
        {
            // this is an event message.

            if (m_status == FULL_FUNCTIONALITY) {
                
                // NEW STUFF, UNTESTED
                // receivers that handle this type of message in this process
                auto it_mt = s_mproc->m_active_handlers.find(m->message_type_id);
                if (it_mt != s_mproc->m_active_handlers.end()) {

                    instance_id_type sids[] = {
                        m->sender_instance_id,
                        any_local,
                        any_local_or_remote
                    };

                    for (auto sid : sids) {
                        auto shl = it_mt->second.find(sid);
                        if (shl != it_mt->second.end()) {
                            for (auto& e : shl->second) {
                                e(*m);
                            }
                        }
                    }

                }
            }
        }
    }

    m_in_req_c->done_reading();

    std::unique_lock<std::mutex> lk(m_req_stop_mutex);
    m_req_running = false;
    m_req_stop_condition.notify_one();
}



inline
void Process_message_reader::reply_reader_function()
{
    m_rep_running = true;

    while (m_status != STOPPING) {
        s_tl_current_message = nullptr;
        Message_prefix* m = m_in_rep_c->fetch_message();
        s_tl_current_message = m;

        if (m == nullptr) {
            break;
        }

        // Only the process with the coordinator's instance is allowed to send messages on
        // someone else's behalf (for relay purposes).
        assert(m_in_rep_c->m_id == process_of(m->sender_instance_id) ||
               m_in_rep_c->m_id == process_of(s_coord_id));

        assert(m->receiver_instance_id != any_local);
        assert(m->message_type_id == not_defined_type_id);

        if (is_local_instance(m->receiver_instance_id)) {

            if ((m_status == FULL_FUNCTIONALITY) ||
                (m->receiver_instance_id == s_coord_id && s_coord) ||
                (m->sender_instance_id   == s_coord_id) )
            {

                auto it = s_mproc->m_local_pointer_of_instance_id.find(m->receiver_instance_id);

                if (it != s_mproc->m_local_pointer_of_instance_id.end()) {
                    auto &return_handlers = it->second->m_active_return_handlers;
                    auto it2 = return_handlers.find(m->function_instance_id);
                    if (it2 != return_handlers.end()) {
                        it2->second.success_handler(*m);
                    }
                    else {
                        // If it exists, there must be a return handler assigned.
                        // This is most likely an error local to this process.
                        assert(!"There is no active handler for the function return.");
                    }
                }
                else {
                    // This can occur by both local and remote error.
                    assert(!"The object that this return message refers to does not exist.");
                }

            }
        }
        else {

            // a specific non-local receiver means an rpc to another process.
            // if the coordinator is in this process, relay
            if (s_coord) {
                // the message type is not specified, thus it is a reply
                s_mproc->m_out_rep_c->relay(*m);
            }
        }
    }

    std::unique_lock<std::mutex> lk(m_rep_stop_mutex);
    m_rep_running = false;
    m_rep_stop_condition.notify_one();
}




}

#endif
