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

#ifndef SINTRA_MANAGED_PROCESS_H
#define SINTRA_MANAGED_PROCESS_H

#include "config.h"
#include "globals.h"
#include "ipc_rings.h"
#include "message.h"
#include "process_message_reader.h"
#include "resolve_type.h"
#include "spinlocked_containers.h"
#include "transceiver.h"
#include "utility/call_function_with_fusion_vector_args.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <float.h>
#include <list>
#include <map>
#include <mutex>
#include <omp.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#include <boost/atomic.hpp>
#include <boost/interprocess/detail/os_thread_functions.hpp>


#undef max



namespace sintra {


using std::condition_variable;
using std::deque;
using std::function;
using std::list;
using std::map;
using std::mutex;
using std::string;
using std::thread;
using std::vector;



struct Entry_descriptor
{
    Entry_descriptor(const string& binary_name)
    {
        m_binary_name = binary_name;
        assert(m_entry_function == nullptr);
    }
    Entry_descriptor(int(*entry_function)())
    {
        m_entry_function = entry_function;
        assert(m_binary_name.empty());
    }
private:
    int (*m_entry_function)() = nullptr;
    string m_binary_name;

    friend struct Managed_process;
};


struct Process_descriptor
{
    Entry_descriptor entry;
    vector<string> sintra_options;
    vector<string> user_options;
    int num_children; // quota

    Process_descriptor(
        const Entry_descriptor& aentry,
        const vector<string>& auser_options = vector<string>(),
        int anum_children = 0)
    :
        entry(aentry),
        user_options(auser_options),
        num_children(anum_children)
    {
    }

};



DECLARE_STATIC_VARIABLE(vector<Process_descriptor>, branch_vector)



 //////////////////////////////////////////////////////////////////////////
///// BEGIN PROCESS GROUP //////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////    //////    //////    //////    //////    //////    //////    //////
 ////      ////      ////      ////      ////      ////      ////      ////
  //        //        //        //        //        //        //        //



template <typename T>
bool& process_group_membership()
{
    static bool m = false;
    return m;
}


template <typename T, type_id_type ID = 0>
struct Process_group
{
    static inline type_id_type id()
    {
        static type_id_type tid = ID ? make_type_id(ID) : sintra::get_type_id<Process_group>();
        return tid;
    }

    static mutex m_barrier_mutex;
    static void barrier();
    static void enroll();
};



template <typename T, type_id_type ID>
mutex Process_group<T, ID>::m_barrier_mutex;


// default process groups
struct All_processes            : Process_group<All_processes> {};
struct Externally_coordinated   : Process_group<Externally_coordinated> {};


  //        //        //        //        //        //        //        //
 ////      ////      ////      ////      ////      ////      ////      ////
//////    //////    //////    //////    //////    //////    //////    //////
////////////////////////////////////////////////////////////////////////////
///// END PROCESS GROUP ////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////



template <typename T>
sintra::type_id_type get_type_id();


template <typename = void>
sintra::instance_id_type get_instance_id(string&& name);



struct Work_loop
{
    thread                              m_thread;
    atomic<bool>                        m_running;
    condition_variable                  m_dirt;
    mutex                               m_mutex;
};



struct Managed_process: Transceiver
{
    TRANSCEIVER_PROLOGUE(Managed_process)

    Managed_process();
    ~Managed_process();

    void init(int argc, char* argv[]);
    void branch();

    void start(int(*entry_function)() = nullptr);
    void stop();
    bool wait_for_stop();


    Message_ring_W*                     m_out_req_c;
    Message_ring_W*                     m_out_rep_c;

    spinlocked_umap<
        string,
        type_id_type
    >                                   m_type_id_of_name;

    spinlocked_umap<
        string,
        instance_id_type
    >                                   m_instance_id_of_name;

    spinlocked_umap<
        instance_id_type,
        Transceiver*
    >                                   m_local_pointer_of_instance_id;

    // START/STOP
    bool                                m_running;
    mutex                               m_start_stop_mutex;
    condition_variable                  m_start_stop_condition;


    template<typename T>
    void deferred_call(double delay, void(T::*v)());

    inline
    void deferred_call(double delay, function<void()> fn);

    template<typename MANAGED_TYPE>
    void manage(MANAGED_TYPE* v);

    inline
    void work_loop();

    inline
    void deferred_insertion_loop();

//:
    Managed_process(Managed_process const&) = delete;
    void operator=(Managed_process const&)  = delete;


    string                              m_binary_name;

    ipc::ipcdetail::OS_process_id_t     m_pid;

    atomic<bool>                        m_must_stop;
    condition_variable                  m_termination_condition;

    thread                              m_message_reading_thread;
    atomic<bool>                        m_message_reading_thread_running;
    

    thread                              m_work_thread;
    atomic<bool>                        m_work_thread_running;
    list<function<void()> >             m_work_items;
    condition_variable                  m_work_items_dirty_condition;
    mutex                               m_work_items_mutex; 

    thread                              m_deferred_insertion_thread;
    atomic<bool>                        m_deferred_insertion_thread_running;

    typedef std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<double> >
        insertion_time_type;

    map<insertion_time_type, function<void()> >
                                        m_deferred_insertion_queue;
    condition_variable                  m_next_deferred_insertion_is_sooner;
    mutex                               m_deferred_mutex;


    bool                                m_branched = false;

    
    struct user_loop
    {
        thread m_thread;
        function<void()> decorated_loop; 
        function<void()> stop; 
    };
    vector<user_loop>                   m_user_loops;

    uint64_t                            m_swarm_id;
    string                              m_directory;

    inline
    string obtain_swarm_directory();


    uint32_t                            m_self_index = -1;

    function<int()> m_entry_function = [&] ()->int { return 0; };
    int                                 m_children_quota = 0;

    sequence_counter_type               m_last_message_sequence;

    // don't be tempted to make this atomic and shared across processes, it's not faster.
    sequence_counter_type               m_check_sequence;

    double                              m_message_stats_reference_time;
    uint64_t                            m_messages_accepted_since_reference_time;
    uint64_t                            m_messages_rejected_since_reference_time;
    sequence_counter_type               m_total_sequences_missed;

    std::chrono::time_point<std::chrono::system_clock>
                                        m_time_instantiated;

    deque<Process_message_reader>       m_readers;
};



} // namespace sintra


#endif
