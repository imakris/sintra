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

#ifndef __SINTRA_MANAGED_PROCESS_IMPL_H__
#define __SINTRA_MANAGED_PROCESS_IMPL_H__


#include "utility.h"

#include <chrono>
#include <mutex>
#include <experimental/filesystem>

#include <boost/lexical_cast.hpp>

#ifdef _WIN32
    #include "third_party/getopt.h"
#else
    #include <getopt.h>
#endif


namespace sintra {


using std::cout;
using std::function;
using std::is_base_of;
using std::lock_guard;
using std::runtime_error;
using std::string;
using std::stringstream;
using std::to_string;
using std::unique_lock;
using std::vector;
namespace fs = std::experimental::filesystem;
namespace chrono = std::chrono;


inline
static void s_signal_handler(int sig)
{
    mproc::s->send<Transceiver::instance_invalidated, any_remote>(mproc_id::s);

    if (coord::s) {
        // should we do something special here?
        // kill every other process?
    }
}


inline
void install_signal_handler()
{
    signal(SIGABRT, s_signal_handler);
    signal(SIGFPE,  s_signal_handler);
    signal(SIGILL,  s_signal_handler);
    signal(SIGINT,  s_signal_handler);
    signal(SIGSEGV, s_signal_handler);
    signal(SIGTERM, s_signal_handler);
}



template <typename T>
sintra::type_id_type get_type_id()
{
    const string name = boost::typeindex::type_id<T>().pretty_name();
    const char* test = name.c_str();
    auto it = mproc::s->m_type_id_of_name.find(name);
    if (it != mproc::s->m_type_id_of_name.end()) {
        return it->second;
    }

    // Caution the Coordinator call will refer to the map that is being assigned,
    // if the Coordinator is local. Do not be tempted to simplify the temporary,
    // because depending on the order of evaluation, it may or it may not work.
    auto tid = Coordinator::rpc_resolve_type(coord_id::s, name);
    return mproc::s->m_type_id_of_name[name] = tid;
}



template <typename>
sintra::instance_id_type get_instance_id(string&& name)
{
    auto it = mproc::s->m_instance_id_of_name.find(name);
    if (it != mproc::s->m_instance_id_of_name.end()) {
        return it->second;
    }


    // Caution the Coordinator call will refer to the map that is being assigned,
    // if the Coordinator is local. Do not be tempted to simplify the temporary,
    // because depending on the order of evaluation, it may or it may not work.
    auto iid = Coordinator::rpc_resolve_instance(coord_id::s, name);
    return mproc::s->m_instance_id_of_name[name] = iid;
}





template <typename T, type_id_type ID>
void Process_group<T, ID>::barrier()
{
    if (!process_group_membership<T>()) {
        throw sintra_logic_error("The process is not a member of that group.");
    }

    // this mutex protects from matching multiple threads on the same process group's barrier
    lock_guard<mutex> barrier_lock(m_barrier_mutex);

    if (!Coordinator::rpc_barrier(coord_id::s, id() )) {
        // This means that another thread must have called the barrier directly as rpc.
        // If this is not the case, then it is a bug.
        throw runtime_error(
            "The barrier was matched by multiple threads of the same process.");
    }
}



template <typename T, type_id_type ID /* = 0*/>
void Process_group<T, ID>::enroll()
{
    if (!Coordinator::rpc_add_this_process_into_group(coord_id::s, id() ) ) {
        throw runtime_error("Attempted to enroll into a process group after branching.");
    }
    process_group_membership<T>() = true;
}



inline
Managed_process::Managed_process():
    Transceiver(0),
    m_must_stop(false),
    m_running(false),
    m_message_reading_thread_running(false),
    m_work_thread_running(false),
    m_deferred_insertion_thread_running(false),
    m_swarm_id(0),
    m_last_message_sequence(0),
    m_check_sequence(0),
    m_message_stats_reference_time(0.),
    m_messages_accepted_since_reference_time(0),
    m_messages_rejected_since_reference_time(0),
    m_total_sequences_missed(0)
{
    assert(mproc::s == nullptr);
    mproc::s = this;

    m_pid = ipc::ipcdetail::get_current_process_id();

    install_signal_handler();

    // NOTE: Do not be tempted to use get_current_process_creation_time from boost::interprocess,
    // it is only implementeded for Windows.
    m_time_instantiated = chrono::system_clock::now();
}



inline
Managed_process::~Managed_process()
{
    if (coord::s) {
        coord::s->wait_until_all_other_processes_are_done();
    }

    assert(!m_running);

    // this is called explicitly, in order to inform the coordinator of the destruction early.
    // it would not be possible to communicate it after the channels were closed.
    this->Transceiver::destroy();

    // no more reading
    m_readers.clear();

    // no more writing
    if (m_out_req_c) {
        delete m_out_req_c;
        m_out_req_c = nullptr;
    }

    if (m_out_rep_c) {
        delete m_out_rep_c;
        m_out_rep_c = nullptr;
    }

    if (coord::s) {

        // now it's safe to delete the Coordinator.
        delete coord::s;

        // removes the swarm directory
        remove_directory(m_directory);
    }

    mproc::s    = nullptr;
    mproc_id::s    = 0;
}



inline
void Managed_process::init(int argc, char* argv[])
{
    m_binary_name = argv[0];
    uint32_t self_index = 0;

    int help_arg = 0;
    string self_index_arg;
    string swarm_id_arg;
    string own_id_arg;
    string coordinator_id_arg;

    try {
        while (true) {
            static struct ::option long_options[] = {
                {"help",            no_argument,        &help_arg,  'h' },
                {"self_index",      required_argument,  0,          'a' },
                {"swarm_id",        required_argument,  0,          'b' },
                {"own_id",          required_argument,  0,          'c' },
                {"coordinator_id",  required_argument,  0,          'd' },
                {0, 0, 0, 0}
            };

            int option_index = 0;
            int c = getopt_long(argc, argv, "ha:b:c:d:", long_options, &option_index);

            if (c == -1)
                break;

            switch (c) {
                case 'h':
                    if (long_options[option_index].flag != 0)
                        throw -1;
                    break;
                case 'a':
                    self_index_arg      = optarg;
                    self_index          = boost::lexical_cast<decltype(self_index   )>(optarg);
                    break;
                case 'b':
                    swarm_id_arg        = optarg;
                    m_swarm_id          = boost::lexical_cast<decltype(m_swarm_id   )>(optarg);
                    break;
                case 'c':
                    own_id_arg          = optarg;
                    m_instance_id       = boost::lexical_cast<decltype(m_instance_id)>(optarg);
                    break;
                case 'd':
                    coordinator_id_arg  = optarg;
                    coord_id::s         = boost::lexical_cast<decltype(coord_id::s  )>(optarg);
                    break;
                case '?':
                    /* getopt_long already printed an error message. */
                    break;
                default :
                    throw -1;
            }
        }
    }
    catch(...) {
        cout << R"(
Managed process options:
  --help                (optional) produce help message and exit
  --self_index arg      used by the coordinator process, when it invokes itself
                        with a different entry index
  --swarm_id arg        unique identifier of the swarm that is being joined
  --own_id arg          the instance id of this process, that was assigned by
                        the supervisor
  --coordinator_id arg  the instance id of this coordinator that this process
                        should refer to
)";
        exit(1);
    }


    bool coordinator_is_local = false;
    if (swarm_id_arg.empty()) {
        mproc_id::s = m_instance_id = make_process_instance_id(1);

        m_swarm_id = 
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_time_instantiated.time_since_epoch()
            ).count();
        m_directory = obtain_swarm_directory();
        coordinator_is_local = true;

        // NOTE: leave m_self_index uninitialised. The supervisor does not have an entry
    }
    else {
        if (!self_index_arg.empty() && !coordinator_id_arg.empty() && !own_id_arg.empty()) {
            assert((self_index >= 0) && (self_index < max_process_instance_id - 1));
            m_self_index = self_index;
            assert(m_instance_id != 0);

            mproc_id::s = m_instance_id;

            m_directory = obtain_swarm_directory();
        }
        else {
            assert(!"if the binary was not invoked manually, this is definitely a bug.");
            exit(1);
        }
    }

    m_out_req_c = new Message_ring_W(m_directory, "req", m_instance_id);
    m_out_rep_c = new Message_ring_W(m_directory, "rep", m_instance_id);

    if (!coordinator_is_local) {
        m_readers.emplace_back(process_of(coord_id::s));
    }

    // now we can proceed with the rest of the initialization

    // why pid: Because this only serves as a unique name, not as an identifier for lookups.
    // It has no other practical use.
    // why m_self_index+2: because Transceiver IDs start from 2:
    // - 0 is invalid
    // - 1 is the first process
    new (static_cast<Transceiver*>(this)) Transceiver(
        "",
        make_process_instance_id(m_self_index + 2)
    );

    if (coordinator_is_local) {
        coord::s = new Coordinator;
        coord_id::s = coord::s->m_instance_id;
        coord::s->add_process_into_group(m_instance_id, All_processes::id());
    }

    process_group_membership<All_processes>() = true;
}


inline
void Managed_process::branch()
{
    using namespace sintra;

    if (coord::s) {

        // 1. prepare the commandline for each invocation
        auto it = branch_vector::s.begin();
        for (int i = 0; it != branch_vector::s.end(); it++, i++) {
            it->sintra_options.push_back("--swarm_id");
            it->sintra_options.push_back(to_string(m_swarm_id));
            if (it->entry.m_binary_name.empty()) {
                it->entry.m_binary_name = m_binary_name;
                it->sintra_options.push_back("--self_index");
                it->sintra_options.push_back(to_string(i));
            }
            auto assigned_instance_id = make_process_instance_id();
            it->sintra_options.push_back("--own_id");
            it->sintra_options.push_back(to_string(assigned_instance_id));
            it->sintra_options.push_back("--coordinator_id");
            it->sintra_options.push_back(to_string(coord_id::s));

            coord::s->add_process_into_group(assigned_instance_id, All_processes::id());
            coord::s->add_process_into_group(assigned_instance_id, Externally_coordinated::id());

            mproc::s->m_readers.emplace_back(assigned_instance_id);
        }

        // 2. spawn
        it = branch_vector::s.begin();
        for (int i = 0; it != branch_vector::s.end(); it++, i++) {

            vector<string> all_args = {it->entry.m_binary_name.c_str()};
            all_args.insert(all_args.end(), it->sintra_options.begin(), it->sintra_options.end());
            all_args.insert(all_args.end(), it->user_options.begin(), it->user_options.end());

            const char** argv = new const char*[all_args.size()+1];
            for (size_t i = 0; i < all_args.size(); i++) {
                argv[i] = all_args[i].c_str();
            }
            argv[all_args.size()] = 0;

            spawn_detached(it->entry.m_binary_name.c_str(), argv);

            delete [] argv;
        }
    }
    else {
        Process_descriptor& own_pd = branch_vector::s[m_self_index];
        if (own_pd.entry.m_entry_function != nullptr) {
            m_entry_function = own_pd.entry.m_entry_function;
        }
        else {
            assert(!"if the binary was not invoked manually, this is definitely a bug.");
            exit(1);
        }

        //m_coordinator links to the coordinator
        // what happens here:
        // the comm will try to start reading fro the ring of m_coordinator_id
        // its write channel is already available
        // the supervisor, which is its own coordinator, will do that too (loop comm)
        //m_peers->read_from(m_coordinator_id);

        process_group_membership<Externally_coordinated>() = true;
    }


    m_branched = true;

    All_processes::barrier();

    assign_name(string("sintra_process_") + to_string(m_pid));
}


 //////////////////////////////////////////////////////////////////////////
///// BEGIN START/STOP /////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////    //////    //////    //////    //////    //////    //////    //////
 ////      ////      ////      ////      ////      ////      ////      ////
  //        //        //        //        //        //        //        //


inline
void Managed_process::start(int(*entry_function)())
{
    m_start_stop_mutex.lock();
    m_running = true;

    for (auto& r : m_readers) {
        r.go();
    }
    m_start_stop_mutex.unlock();

    if (!entry_function) {
        m_entry_function();
    }
    else {
        mproc::s->m_readers.emplace_back(mproc_id::s);
        entry_function();
    }

    stop();
}



inline
void Managed_process::stop()
{
    lock_guard<mutex> start_stop_lock(m_start_stop_mutex);

    // stop() might be call either by start() when the entry function finishes execution,
    // or explicitly from one of the handlers, or the entry function itself.
    // If called when the process is already stopped, this should not cause problems.
    if (!m_running)
        return;

    // call each user defined loop's stop function
    for (auto& v : m_user_loops)
        v.stop();


    /*

    m_must_stop.store(true);

    //m_mr_control->dirty_condition.notify_all();
    m_next_deferred_insertion_is_sooner.notify_all();
    m_work_items_dirty_condition.notify_all();

    boost::unique_lock<boost::mutex> lock(m_work_items_mutex);
    while (m_message_reading_thread_running.load() ||
        m_work_thread_running.load() ||
        m_deferred_insertion_thread_running.load())
    {
        m_termination_condition.wait(lock);
    }

    m_deferred_insertion_thread.join();
    m_message_reading_thread.join();
    m_work_thread.join();

    */


    for (auto& i : m_readers) {
        i.suspend();
    }

    m_running = false;

    m_start_stop_condition.notify_all();
}


inline
bool Managed_process::wait_for_stop()
{
    unique_lock<mutex> start_stop_lock(m_start_stop_mutex);
    bool running_state_at_entry = m_running;
    while (m_running) {
        m_start_stop_condition.wait(start_stop_lock);
    }
    return running_state_at_entry;
}


  //        //        //        //        //        //        //        //
 ////      ////      ////      ////      ////      ////      ////      ////
//////    //////    //////    //////    //////    //////    //////    //////
////////////////////////////////////////////////////////////////////////////
///// END START/STOP ///////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////




template<typename T>
void Managed_process::deferred_call(double delay, void(T::*v)())
{
    BOOST_STATIC_ASSERT(is_base_of<Managed_process, T>::value);
    deferred_call(delay, boost::bind(v, static_cast<T*>(this)));
}

inline
void Managed_process::deferred_call(double delay, function<void()> fn)
{
    unique_lock<mutex> deferred_queue_lock(m_deferred_mutex);
    auto when = chrono::steady_clock::now() + chrono::duration<double>(delay);
    auto it = m_deferred_insertion_queue.insert(make_pair(when, fn)).first;
    if (it == m_deferred_insertion_queue.begin()) {
        m_next_deferred_insertion_is_sooner.notify_all();
    }
}

template<typename MANAGED_TYPE>
void Managed_process::manage(MANAGED_TYPE* v)
{
    user_loop ul;
    // make thread with the user defined main loop, decorated as needed
    // (i.e. with signal handler and termination condition notifier).
    ul.decorated_loop = [&]() {
        install_signal_handler();
        v->loop();
        stop();
    };
    ul.stop = [&]() {
        v->stop();
    };
    m_user_loops.push_back(ul);
}



inline
void Managed_process::work_loop()
{
    install_signal_handler();
    m_work_thread_running.store(true);

    while (!m_must_stop.load()) {
        unique_lock<mutex> work_items_lock(m_work_items_mutex);
        // wait until there is some work to do
        if (m_work_items.empty()) {
            m_work_items_dirty_condition.wait(work_items_lock);

            // the above condition can unblock even if there are no calls
            continue;
        }

        auto& work_item = m_work_items.front();
        work_items_lock.unlock();
        work_item();
        work_items_lock.lock();
        m_work_items.pop_front();
    }

    m_work_thread_running.store(false);
    m_termination_condition.notify_all();
}

inline
void Managed_process::deferred_insertion_loop()
{
    install_signal_handler();
    m_deferred_insertion_thread_running.store(true);
    insertion_time_type time_of_next_insertion =
        chrono::steady_clock::now() + chrono::duration<double>(1000);

    unique_lock<mutex> deferred_queue_lock(m_deferred_mutex);

    if (m_deferred_insertion_queue.empty()) {
        m_next_deferred_insertion_is_sooner.wait_until(deferred_queue_lock, time_of_next_insertion);
    }

    while (!m_must_stop.load()) {

        // keep inserting, while now() is later than the first element's
        // insertion time and m_deferred_queue is not empty.
        do {
            if (m_deferred_insertion_queue.empty()) {
                time_of_next_insertion =
                    chrono::steady_clock::now() + chrono::duration<double>(1000);
                break;
            }
            auto element_insertion_time = m_deferred_insertion_queue.begin()->first;
            if (chrono::steady_clock::now() < element_insertion_time) {
                time_of_next_insertion = element_insertion_time;
                break;
            }
            unique_lock<mutex> work_items_lock(m_work_items_mutex);
            m_work_items.push_back(m_deferred_insertion_queue.begin()->second);
            m_work_items_dirty_condition.notify_all();
            work_items_lock.unlock();
            m_deferred_insertion_queue.erase(m_deferred_insertion_queue.begin());
        }
        while (true);

        m_next_deferred_insertion_is_sooner.wait_until(deferred_queue_lock, time_of_next_insertion);
    }

    m_deferred_insertion_thread_running.store(false);
    m_termination_condition.notify_all();
}


inline
string Managed_process::obtain_swarm_directory()
{
    string sintra_directory = fs::temp_directory_path().string() + "/sintra/";
    if (!check_or_create_directory(sintra_directory)) {
        throw runtime_error("access to a working directory failed");
    }

    stringstream stream;
    stream << std::hex << m_swarm_id;
    auto swarm_directory = sintra_directory + stream.str();
    if (!check_or_create_directory(swarm_directory)) {
        throw runtime_error("access to a working directory failed");
    }

    return swarm_directory;
}




} // sintra


#endif
