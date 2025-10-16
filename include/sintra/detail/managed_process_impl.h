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

#ifndef SINTRA_MANAGED_PROCESS_IMPL_H
#define SINTRA_MANAGED_PROCESS_IMPL_H


#include "utility.h"
#include "debug_log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <list>
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <thread>
#include <utility>
#ifndef _WIN32
#include <signal.h>
#include <cerrno>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

#include <boost/type_index/ctti_type_index.hpp>

namespace sintra {

extern thread_local bool tl_is_req_thread;
extern thread_local std::function<void()> tl_post_handler_function;

inline std::once_flag& signal_handler_once_flag()
{
    static std::once_flag flag;
    return flag;
}

namespace {

#ifdef _WIN32
    struct signal_slot {
        int sig;
        void (__cdecl* previous)(int) = SIG_DFL;
        bool has_previous = false;
    };

    inline std::array<signal_slot, 6>& signal_slots()
    {
        static std::array<signal_slot, 6> slots {{
            {SIGABRT}, {SIGFPE}, {SIGILL}, {SIGINT}, {SIGSEGV}, {SIGTERM}
        }};
        return slots;
    }
#else
    struct signal_slot {
        int sig;
        struct sigaction previous {};
        bool has_previous = false;
    };

    inline std::array<signal_slot, 6>& signal_slots()
    {
        static std::array<signal_slot, 6> slots {{
            {SIGABRT}, {SIGFPE}, {SIGILL}, {SIGINT}, {SIGSEGV}, {SIGTERM}
        }};
        return slots;
    }

    inline std::array<char, 64 * 1024>& alt_stack_storage()
    {
        static std::array<char, 64 * 1024> storage {};
        return storage;
    }

    inline bool& alt_stack_installed()
    {
        static bool installed = false;
        return installed;
    }

    inline int (&signal_pipe())[2]
    {
        static int pipefd[2] = {-1, -1};
        return pipefd;
    }

    inline std::atomic<unsigned int>& pending_signal_mask()
    {
        static std::atomic<unsigned int> mask {0};
        return mask;
    }

    inline std::atomic<uint32_t>& dispatched_signal_counter()
    {
        static std::atomic<uint32_t> counter {0};
        return counter;
    }

    inline std::once_flag& signal_dispatcher_once_flag()
    {
        static std::once_flag flag;
        return flag;
    }

    inline std::size_t signal_index(int sig)
    {
        auto& slots = signal_slots();
        for (std::size_t idx = 0; idx < slots.size(); ++idx) {
            if (slots[idx].sig == sig) {
                return idx;
            }
        }
        return slots.size();
    }

    inline void dispatch_signal_number(int sig_number)
    {
        auto* mproc = s_mproc;
        if (mproc && mproc->m_out_req_c) {
            mproc->emit_remote<Managed_process::terminated_abnormally>(sig_number);

            std::shared_lock<std::shared_mutex> readers_lock(mproc->m_readers_mutex);
            for (auto& reader : mproc->m_readers) {
                reader.second.stop_nowait();
            }

            dispatched_signal_counter().fetch_add(1, std::memory_order_release);
        }
    }

    inline void wait_for_signal_dispatch(uint32_t expected_count)
    {
        const uint32_t target = expected_count + 1;
        timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1'000'000; // 1 millisecond
        for (int spin = 0; spin < 200; ++spin) {
            if (dispatched_signal_counter().load(std::memory_order_acquire) >= target) {
                return;
            }
            ::nanosleep(&ts, nullptr);
        }
    }

    inline void drain_pending_signals()
    {
        auto mask = pending_signal_mask().exchange(0U, std::memory_order_acq_rel);
        if (mask == 0U) {
            return;
        }

        auto& slots = signal_slots();
        for (std::size_t idx = 0; idx < slots.size(); ++idx) {
            if ((mask & (1U << idx)) != 0U) {
                dispatch_signal_number(slots[idx].sig);
            }
        }
    }

    inline void signal_dispatch_loop()
    {
        auto& pipefd = signal_pipe();

        while (true) {
            int sig_number = 0;
            ssize_t bytes_read = ::read(pipefd[0], &sig_number, sizeof(sig_number));

            if (bytes_read == sizeof(sig_number)) {
                dispatch_signal_number(sig_number);
                drain_pending_signals();
                continue;
            }

            if (bytes_read == -1) {
                if (errno == EINTR) {
                    drain_pending_signals();
                    continue;
                }

                if (errno == EAGAIN) {
                    std::this_thread::yield();
                    drain_pending_signals();
                    continue;
                }
            }

            drain_pending_signals();

            // Either the write end was closed (bytes_read == 0) or an unrecoverable
            // error occurred. Exit the loop to allow the thread to finish.
            break;
        }
    }

    inline void ensure_signal_dispatcher()
    {
        std::call_once(signal_dispatcher_once_flag(), []() {
            int pipefd_local[2];
            if (::pipe(pipefd_local) != 0) {
                return;
            }

            int flags = ::fcntl(pipefd_local[1], F_GETFL, 0);
            if (flags != -1) {
                ::fcntl(pipefd_local[1], F_SETFL, flags | O_NONBLOCK);
            }

            auto& pipefd = signal_pipe();
            pipefd[0] = pipefd_local[0];
            pipefd[1] = pipefd_local[1];

            std::thread(signal_dispatch_loop).detach();
        });
    }
#endif

    inline signal_slot* find_slot(std::array<signal_slot, 6>& slots, int sig)
    {
        for (auto& candidate : slots) {
            if (candidate.sig == sig) {
                return &candidate;
            }
        }
        return nullptr;
    }
}



#ifdef _WIN32
inline
static void s_signal_handler(int sig)
{
    if (s_mproc && s_mproc->m_out_req_c) {
        s_mproc->emit_remote<Managed_process::terminated_abnormally>(sig);

        std::shared_lock<std::shared_mutex> readers_lock(s_mproc->m_readers_mutex);
        for (auto& reader : s_mproc->m_readers) {
            reader.second.stop_nowait();
        }
    }

    // On Windows, forcefully terminate the process to avoid deadlock during shutdown.
    // Reader threads may be blocked on semaphores, and Windows shutdown waits for
    // all threads to exit. Since this is a crashing process (signal handler was called),
    // we don't need graceful shutdown - the coordinator will detect the death and
    // respawn if recovery is enabled. Mutex recovery will handle any abandoned locks.
    TerminateProcess(GetCurrentProcess(), 1);
}
#else
inline
static void s_signal_handler(int sig, siginfo_t* info, void* ctx)
{
    auto& slots = signal_slots();

    auto* mproc = s_mproc;
    const bool should_wait_for_dispatch = mproc && mproc->m_out_req_c;
    uint32_t dispatched_before = 0;
    if (should_wait_for_dispatch) {
        dispatched_before = dispatched_signal_counter().load(std::memory_order_relaxed);
    }

    auto& pipefd = signal_pipe();
    if (pipefd[1] != -1) {
        int sig_number = sig;
        int last_errno = 0;
        bool delivered = false;
        constexpr int max_attempts = 4;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            ssize_t result = ::write(pipefd[1], &sig_number, sizeof(sig_number));
            if (result == sizeof(sig_number)) {
                delivered = true;
                break;
            }

            if (result == -1) {
                last_errno = errno;
                if (last_errno == EINTR || last_errno == EAGAIN) {
                    continue;
                }
            }

            break;
        }

        if (!delivered && (last_errno == EAGAIN || last_errno == EINTR)) {
            auto index = signal_index(sig);
            if (index < slots.size()) {
                pending_signal_mask().fetch_or(1U << index, std::memory_order_release);
            }
        }
    }

    if (should_wait_for_dispatch) {
        wait_for_signal_dispatch(dispatched_before);
    }

    if (auto* slot = find_slot(slots, sig); slot && slot->has_previous) {
        if (slot->previous.sa_handler == SIG_IGN) {
            return;
        }

        if ((slot->previous.sa_flags & SA_SIGINFO) && slot->previous.sa_sigaction) {
            slot->previous.sa_sigaction(sig, info, ctx);
            return;
        }

        if (slot->previous.sa_handler && slot->previous.sa_handler != SIG_DFL) {
            slot->previous.sa_handler(sig);
            return;
        }
    }

    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    raise(sig);
}
#endif


inline
void Managed_process::enable_recovery()
{
    // Mark this process as recoverable in the coordinator
    // so that abnormal termination triggers a respawn.
    Coordinator::rpc_enable_recovery(s_coord_id, process_of(m_instance_id));
    m_recoverable = true;
}


inline
void install_signal_handler()
{
    std::call_once(signal_handler_once_flag(), []() {
        auto& slots = signal_slots();

#ifdef _WIN32
        for (auto& slot : slots) {
            auto previous = std::signal(slot.sig, s_signal_handler);
            if (previous != SIG_ERR) {
                slot.has_previous = previous != s_signal_handler;
                slot.previous = slot.has_previous ? previous : SIG_DFL;
            }
            else {
                slot.has_previous = false;
            }
        }
#else
        auto& storage = alt_stack_storage();
        auto& installed = alt_stack_installed();
        if (!installed) {
            stack_t ss {};
            ss.ss_sp = storage.data();
            ss.ss_size = storage.size();
            ss.ss_flags = 0;
            if (sigaltstack(&ss, nullptr) == 0) {
                installed = true;
            }
        }

        ensure_signal_dispatcher();

        for (auto& slot : slots) {
            struct sigaction sa {};
            sigemptyset(&sa.sa_mask);
            sa.sa_sigaction = s_signal_handler;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
#ifdef SA_RESTART
            sa.sa_flags |= SA_RESTART;
#endif
            if (sigaction(slot.sig, &sa, &slot.previous) == 0) {
                slot.has_previous = slot.previous.sa_sigaction != s_signal_handler;
                if (!slot.has_previous) {
                    slot.previous.sa_handler = SIG_DFL;
                }
            }
            else {
                slot.has_previous = false;
            }
        }
#endif
    });
}


template <typename T>
sintra::type_id_type get_type_id()
{
    const std::string pretty_name = boost::typeindex::ctti_type_index::template type_id<T>().pretty_name();
    auto it = s_mproc->m_type_id_of_type_name.find(pretty_name);
    if (it != s_mproc->m_type_id_of_type_name.end()) {
        return it->second;
    }

    // Caution the Coordinator call will refer to the map that is being assigned,
    // if the Coordinator is local. Do not be tempted to simplify the temporary,
    // because depending on the order of evaluation, it may or it may not work.
    auto tid = Coordinator::rpc_resolve_type(s_coord_id, pretty_name);

    // if it is not invalid, cache it
    if (tid != invalid_type_id) {
        s_mproc->m_type_id_of_type_name[pretty_name] = tid;
    }

    return tid;
}

// helper
template <typename T>
sintra::type_id_type get_type_id(const T&) {return get_type_id<T>();}


template <typename>
sintra::instance_id_type get_instance_id(std::string&& assigned_name)
{
    auto it = s_mproc->m_instance_id_of_assigned_name.find(assigned_name);
    if (it != s_mproc->m_instance_id_of_assigned_name.end()) {
        return it->second;
    }

    // Caution the Coordinator call will refer to the map that is being assigned,
    // if the Coordinator is local. Do not be tempted to simplify the temporary,
    // because depending on the order of evaluation, it may or it may not work.
    auto iid = Coordinator::rpc_resolve_instance(s_coord_id, assigned_name);

    // if it is not invalid, cache it
    if (iid != invalid_instance_id) {
        s_mproc->m_instance_id_of_assigned_name[assigned_name] = iid;
    }

    return iid;
}



inline
Managed_process::Managed_process():
    Derived_transceiver<Managed_process>((void*)0),
    m_communication_state(COMMUNICATION_STOPPED),
    m_must_stop(false),
    m_swarm_id(0),
    m_last_message_sequence(0),
    m_check_sequence(0),
    m_message_stats_reference_time(0.),
    m_messages_accepted_since_reference_time(0),
    m_messages_rejected_since_reference_time(0),
    m_total_sequences_missed(0)
{
    assert(s_mproc == nullptr);
    s_mproc = this;

    m_pid = ipc::ipcdetail::get_current_process_id();

    install_signal_handler();

    // NOTE: Do not be tempted to use get_current_process_creation_time from boost::interprocess,
    // it is only implemented for Windows.
    m_time_instantiated = std::chrono::system_clock::now();
}



inline
Managed_process::~Managed_process()
{
    // The coordinating process will be removing its readers whenever
    // they are unpublished - when they are all done, the process may exit.
    //
    // Previously we waited for readers to report completion before asking
    // them to stop.  On Linux this could deadlock during shutdown because
    // the reader threads were still blocked on their rings, so the
    // m_num_active_readers counter never reached the expected value and the
    // destructor blocked forever.  Stop the readers first so they can wake
    // up, decrement the counter and unblock the wait.  When invoked from a
    // request reader thread, make sure the deferred reply-ring shutdown runs
    // before waiting and allow for the current thread in the count so we do
    // not deadlock on ourselves.
    if (s_coord) {
        stop();

        const bool called_from_request_reader = tl_is_req_thread;
        if (called_from_request_reader && tl_post_handler_function) {
            auto post_handler = std::move(tl_post_handler_function);
            tl_post_handler_function = {};
            if (post_handler) {
                post_handler();
            }
        }

        wait_until_all_external_readers_are_done(called_from_request_reader ? 1 : 0);
    }

    assert(m_communication_state <= COMMUNICATION_PAUSED); // i.e. paused or stopped

    // this is called explicitly, in order to inform the coordinator of the destruction early.
    // it would not be possible to communicate it after the channels were closed.
    this->Derived_transceiver<Managed_process>::destroy();

    // no more reading
    {
        std::unique_lock<std::shared_mutex> readers_lock(m_readers_mutex);
        m_readers.clear();
    }

    // no more writing
    if (m_out_req_c) {
        delete m_out_req_c;
        m_out_req_c = nullptr;
    }

    if (m_out_rep_c) {
        delete m_out_rep_c;
        m_out_rep_c = nullptr;
    }

    if (s_coord) {

        // now it's safe to delete the Coordinator.
        delete s_coord;
        s_coord = 0;

        // removes the swarm directory
        remove_directory(m_directory);
    }

#ifndef _WIN32
    // Close the signal dispatch pipe to allow the dispatch thread to exit cleanly
    auto& pipefd = signal_pipe();
    if (pipefd[1] != -1) {
        ::close(pipefd[1]);
        pipefd[1] = -1;
    }
    if (pipefd[0] != -1) {
        ::close(pipefd[0]);
        pipefd[0] = -1;
    }
#endif

    s_mproc = nullptr;
    s_mproc_id = 0;
}



// returns the argc/argv as a vector of strings
inline
std::vector<std::string> argc_argv_to_vector(int argc, const char* const* argv)
{
    std::vector<std::string> ret;
    for (int i = 0; i < argc; i++) {
        ret.push_back(argv[i]);
    }
    return ret;
}



struct Filtered_args
{
    vector<string> remained;
    vector<string> extracted;
};


inline std::string join_strings(const std::vector<std::string>& parts, const std::string& delimiter)
{
    if (parts.empty()) {
        return std::string();
    }

    std::string result;
    size_t total_size = delimiter.size() * (parts.size() - 1);
    for (const auto& part : parts) {
        total_size += part.size();
    }
    result.reserve(total_size);

    auto it = parts.begin();
    result += *it++;
    for (; it != parts.end(); ++it) {
        result += delimiter;
        result += *it;
    }

    return result;
}



inline
Filtered_args filter_option(
    std::vector<std::string> in_args, std::string in_option, unsigned int num_args
)
{
    Filtered_args ret;
    bool found = false;
    for (auto& e : in_args) {
        if (!found) {
            if (e == in_option) {
                found = true;
                ret.extracted.push_back(e);
                continue;
            }
            ret.remained.push_back(e);
        }
        else {
            if (num_args) {
                ret.extracted.push_back(e);
                num_args--;
            }
            else {
                ret.remained.push_back(e);
            }
        }
    }
    return ret;
}



inline
void Managed_process::init(int argc, const char* const* argv)
{
    m_binary_name = argv[0];

    std::string branch_index_arg;
    std::string swarm_id_arg;
    std::string instance_id_arg;
    std::string coordinator_id_arg;
    std::string recovery_arg;

    size_t recovery_occurrence_value = 0;
    auto fa = filter_option(argc_argv_to_vector(argc, argv), "--recovery_occurrence", 1);

    if (fa.extracted.size() == 2) {
        try {
            recovery_occurrence_value =  std::stoul(fa.extracted[1]);
        }
        catch (...) {
            assert(!"not implemented");
        }
    }

    m_recovery_cmd = join_strings(fa.remained, " ") + " --recovery_occurrence " +
        std::to_string(recovery_occurrence_value+1);

    auto option_value = [&](const std::string& arg, const char* long_name, char short_name, bool requires_value, int& index) -> std::optional<std::string> {
        const std::string long_prefix = std::string(long_name) + "=";

        if (arg == long_name) {
            if (!requires_value) {
                return std::string();
            }
            if (index + 1 >= argc) {
                throw 1;
            }
            return std::string(argv[++index]);
        }

        if (requires_value && arg.rfind(long_prefix, 0) == 0) {
            return arg.substr(long_prefix.size());
        }

        if (short_name != '\0' && arg.size() >= 2 && arg[0] == '-' && arg[1] == short_name) {
            if (!requires_value) {
                return std::string();
            }

            if (arg.size() > 2) {
                if (arg[2] == '=') {
                    return arg.substr(3);
                }
                return arg.substr(2);
            }

            if (index + 1 >= argc) {
                throw 1;
            }
            return std::string(argv[++index]);
        }

        return std::nullopt;
    };

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "--help" || arg == "-h") {
                throw 1;
            }

            if (auto value = option_value(arg, "--branch_index", 'a', true, i)) {
                try {
                    branch_index_arg = *value;
                    s_branch_index = static_cast<int32_t>(std::stol(*value));
                }
                catch (...) {
                    throw 1;
                }

                if (s_branch_index < 1) {
                    throw 1;
                }

                continue;
            }

            if (auto value = option_value(arg, "--swarm_id", 'b', true, i)) {
                try {
                    swarm_id_arg = *value;
                    m_swarm_id = static_cast<decltype(m_swarm_id)>(std::stoull(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            if (auto value = option_value(arg, "--instance_id", 'c', true, i)) {
                try {
                    instance_id_arg = *value;
                    m_instance_id = static_cast<decltype(m_instance_id)>(std::stoull(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            if (auto value = option_value(arg, "--coordinator_id", 'd', true, i)) {
                try {
                    coordinator_id_arg = *value;
                    s_coord_id = static_cast<instance_id_type>(std::stoull(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            if (auto value = option_value(arg, "--recovery_occurrence", 'e', true, i)) {
                try {
                    recovery_arg = *value;
                    s_recovery_occurrence = static_cast<uint32_t>(std::stoul(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            if (!arg.empty() && arg[0] == '-') {
                // Ignore unknown options so the examples can run under environments
                // that inject additional debugger flags (e.g. Visual Studio).
                continue;
            }
        }
    }
    catch(...) {
        cout << R"(
Managed process options:
  --help                   (optional) produce help message and exit
  --branch_index arg       used by the coordinator process, when it invokes
                           itself
                           with a different entry index. It must be 1 or
                           greater.
  --swarm_id arg           unique identifier of the swarm that is being joined
  --instance_id arg        the instance id assigned to the new process
  --coordinator_id arg     the instance id of the coordinator that this
                           process should refer to
  --recovery_occurrence arg (optional) number of times the process recovered an
                           abnormal termination.
)";
        exit(1);
    }

    bool coordinator_is_local = false;
    if (swarm_id_arg.empty()) {
        s_mproc_id = m_instance_id = make_process_instance_id();

        m_swarm_id = 
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_time_instantiated.time_since_epoch()
            ).count();
        coordinator_is_local = true;

        // NOTE: leave s_branch_index uninitialized. The coordinating process does
        // not have an entry
    }
    else {
        if (coordinator_id_arg.empty() || (branch_index_arg.empty() && instance_id_arg.empty()) ) {

            assert(!"if the binary was not invoked manually, this is definitely a bug.");
            exit(1);
        }

        if (!branch_index_arg.empty()) {
            assert(s_branch_index < max_process_index - 1);
        }

        assert(m_instance_id != 0);

        if (instance_id_arg.empty()) {
            // If branch_index is specified, the explicit instance_id may not, thus
            // we need to make a process instance id based on the branch_index.
            // Transceiver IDs start from 2 (0 is invalid, 1 is the first process)
            m_instance_id = make_process_instance_id(s_branch_index + 2);
        }

        s_mproc_id = m_instance_id;
    }
    m_coordinator_is_local = coordinator_is_local;
    std::cerr << "[Managed_process] instantiated process " << m_instance_id
              << " coordinator_local="
              << (coordinator_is_local ? "true" : "false")
              << " swarm_id=" << m_swarm_id << std::endl;
    detail::trace_sync("managed.init", [&](auto& os) {
        os << "instance=" << m_instance_id
           << " coordinator_local=" << coordinator_is_local
           << " swarm=" << m_swarm_id
           << " binary=" << m_binary_name;
    });
    m_directory = obtain_swarm_directory();

    m_out_req_c = new Message_ring_W(m_directory, "req", m_instance_id, s_recovery_occurrence);
    m_out_rep_c = new Message_ring_W(m_directory, "rep", m_instance_id, s_recovery_occurrence);

    if (coordinator_is_local) {
        s_coord = new Coordinator;
        s_coord_id = s_coord->m_instance_id;

        {
            lock_guard<mutex> lock(s_coord->m_publish_mutex);
            s_coord->m_transceiver_registry[s_mproc_id];
        }
    }

    {
        std::unique_lock<std::shared_mutex> readers_lock(m_readers_mutex);
        assert(!m_readers.count(process_of(s_coord_id)));
        auto progress = std::make_shared<Process_message_reader::Delivery_progress>();
        auto [reader_it, inserted] = m_readers.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(process_of(s_coord_id)),
            std::forward_as_tuple(process_of(s_coord_id), progress, 0u));
        assert(inserted == true);
        reader_it->second.wait_until_ready();
    }

    // Up to this point, there was no infrastructure for a proper construction
    // of Transceiver base.

    this->Derived_transceiver<Managed_process>::construct("", m_instance_id);

    auto published_handler = [this](const Coordinator::instance_published& msg)
    {
        tn_type tn = {msg.type_id, msg.assigned_name};

        while (true) {
            function<void()> next_call;
            {
                lock_guard<mutex> lock(m_availability_mutex);

                auto it = m_queued_availability_calls.find(tn);
                if (it == m_queued_availability_calls.end()) {
                    return;
                }

                if (it->second.empty()) {
                    m_queued_availability_calls.erase(it);
                    return;
                }

                next_call = it->second.front();
            }

            // each function call, which is a lambda defined inside
            // call_on_availability(), clears itself from the list as well.
            next_call();
        }
    };


    auto unpublished_handler = [this](const Coordinator::instance_unpublished& msg)
    {
        auto iid = msg.instance_id;
        auto process_iid = process_of(iid);
        if (iid == process_iid) {

            // the unpublished transceiver was a process, thus we should
            // remove all transceiver records who are known to live in it.

            auto name_map = m_instance_id_of_assigned_name.scoped();
            for (auto it = name_map.begin(); it != name_map.end();) {
                if (process_of(it->second) == iid) {
                    it = name_map.erase(it);
                }
                else {
                    ++it;
                }
            }

            s_mproc->unblock_rpc(iid);
        }

        // if the unpublished transceiver is the coordinator process, we have to stop.
        if (process_of(s_coord_id) == msg.instance_id) {
            // Coordinator process has unpublished - pause communication outside the handler
            // to avoid reentrancy into barrier machinery.
            s_mproc->run_after_current_handler([]{
                s_mproc->stop(); // idempotent
            });
        }
    };

    if (!s_coord) {
        activate(published_handler,   Typed_instance_id<Coordinator>(s_coord_id));
        activate(unpublished_handler, Typed_instance_id<Coordinator>(s_coord_id));
    }

    if (coordinator_is_local) {
        auto unpublish_notify_handler = [](const Managed_process::unpublish_transceiver_notify& msg)
        {
            if (s_coord) {
                s_coord->unpublish_transceiver_notify(msg.transceiver_instance_id);
            }
        };

        auto cr_handler = [](const Managed_process::terminated_abnormally& msg)
        {
            s_coord->unpublish_transceiver(msg.sender_instance_id);

            s_coord->recover_if_required(msg.sender_instance_id);

            /*
            
            There is a problem here:
            We reach this code in the ring reader of the process that crashed.
            The message is then relayed to the coordinator ring
            but the coordinating process reads its own ring too
            which will cause this to be handled twice... which is wrong

            a fix would be to simply reject the message here, in one of the two cases
            but this is not the only case that such a problem could occur. It thus needs a better solution.
            Also, this solution would require declaring some thread-local stuff, otherwise
            knowing whether we are reading the coordinator is impossible.

            maybe an alternative would be that the coordinating process does not handle such messages
            unless they are originating from its own ring. This would however require this to be checked
            in the message loop, which would make it slower for ALL processes - maybe not a good idea.

            another possible alternative (that requires more thought) is that the coordinating process
            does not read its own ring. There might be several corner cases which won't work.

            */

        };
        activate<Managed_process>(unpublish_notify_handler, any_remote);
        activate<Managed_process>(cr_handler, any_remote);
    }
    else {
        auto cr_handler = [](const Managed_process::terminated_abnormally& msg)
        {
            // Remote coordinator crashed: fail outstanding RPCs and pause communication
            if (process_of(s_coord_id) == msg.sender_instance_id) {
                // 1) Wake any RPCs waiting on the coordinator so they fail fast.
                s_mproc->unblock_rpc(process_of(s_coord_id));
                // 2) Pause communication *after* this handler completes to avoid reentrancy.
                s_mproc->run_after_current_handler([]{
                    s_mproc->stop(); // idempotent
                });
            }
        };
        activate<Managed_process>(cr_handler, any_remote);
    }

    m_start_stop_mutex.lock();

    m_communication_state = COMMUNICATION_RUNNING;
    m_start_stop_mutex.unlock();
}



inline
bool Managed_process::spawn_swarm_process(
    const Spawn_swarm_process_args& s )
{
    assert(s_coord);
    std::vector<instance_id_type> failed_spawns, successful_spawns;
    auto args = s.args;
    args.insert(args.end(), {"--recovery_occurrence", std::to_string(s.occurrence)} );

    cstring_vector cargs(args);

    {
        std::unique_lock<std::shared_mutex> readers_lock(m_readers_mutex);

        // If a reader for this process id exists (from a previous crashed instance),
        // stop it and remove it before creating a fresh one for recovery.
        if (auto existing = m_readers.find(s.piid); existing != m_readers.end()) {
            existing->second.stop_and_wait(1.0);
            m_readers.erase(existing);
        }

        auto progress = std::make_shared<Process_message_reader::Delivery_progress>();
        auto eit = m_readers.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(s.piid),
            std::forward_as_tuple(s.piid, progress, s.occurrence)
        );
        assert(eit.second == true);

        // Before spawning the new process, we have to assure that the
        // corresponding reading threads are up and running.
        eit.first->second.wait_until_ready();
        detail::trace_sync("spawn.reader_ready", [&](auto& os) {
            os << "parent_instance=" << m_instance_id
               << " child_instance=" << s.piid
               << " occurrence=" << s.occurrence;
        });
    }

    bool success = spawn_detached(s.binary_name.c_str(), cargs.v());
    detail::trace_sync("spawn.exec", [&](auto& os) {
        os << "parent_instance=" << m_instance_id
           << " child_instance=" << s.piid
           << " occurrence=" << s.occurrence
           << " success=" << success
           << " binary=" << s.binary_name;
    });

    if (success) {
        // Create an entry in the coordinator's transceiver registry.
        // This is essential for the implementation of publish_transceiver()
        {
            lock_guard<mutex> lock(s_coord->m_publish_mutex);
            s_coord->m_transceiver_registry[s.piid];
        }

        // create the readers. The next line will start the reader threads,
        // which might take some time. At this stage, we do not have to wait
        // until they are ready for messages.
        //m_readers.emplace_back(std::move(reader));

        m_cached_spawns[s.piid] = s;
        m_cached_spawns[s.piid].occurrence++;
    }
    else {
        std::cerr << "failed to launch " << s.binary_name << std::endl;

        //m_readers.pop_back();
        std::unique_lock<std::shared_mutex> readers_lock(m_readers_mutex);
        m_readers.erase(s.piid);
    }

    return success;
}



inline
bool Managed_process::branch(vector<Process_descriptor>& branch_vector)
{
    // this function may only be called when a group of processes start.
    assert(!branch_vector.empty());

    using namespace sintra;
    using std::to_string;

    if (s_coord) {

        // 1. prepare the command line for each invocation
        auto it = branch_vector.begin();
        for (int i = 1; it != branch_vector.end(); it++, i++) {

            auto& options = it->sintra_options;
            if (it->entry.m_binary_name.empty()) {
                it->entry.m_binary_name = m_binary_name;
                options.insert(options.end(), { "--branch_index", to_string(i) });
            }
            options.insert(options.end(), {
                "--swarm_id",       to_string(m_swarm_id),
                "--instance_id",    to_string(it->assigned_instance_id = make_process_instance_id()),
                "--coordinator_id", to_string(s_coord_id)
            });
        }

        // 2. spawn
        std::unordered_set<instance_id_type> successfully_spawned;
        it = branch_vector.begin();
        //auto readers_it = m_readers.begin();
        for (int i = 0; it != branch_vector.end(); it++, i++) {

            std::vector<std::string> all_args = {it->entry.m_binary_name.c_str()};
            all_args.insert(all_args.end(), it->sintra_options.begin(), it->sintra_options.end());
            all_args.insert(all_args.end(), it->user_options.begin(), it->user_options.end());

            if (spawn_swarm_process({it->entry.m_binary_name, all_args, it->assigned_instance_id})) {
                successfully_spawned.insert(it->assigned_instance_id);
            }
        }

        auto all_processes = successfully_spawned;
        all_processes.insert(m_instance_id);

        m_group_all      = s_coord->make_process_group("_sintra_all_processes", all_processes);
        m_group_external = s_coord->make_process_group("_sintra_external_processes", successfully_spawned);

        s_branch_index = 0;
    }
    else {
        assert(s_branch_index != -1);
        assert( (ptrdiff_t)branch_vector.size() > s_branch_index-1);
        Process_descriptor& own_pd = branch_vector[s_branch_index-1];
        if (own_pd.entry.m_entry_function != nullptr) {
            m_entry_function = own_pd.entry.m_entry_function;
        }
        else {
            assert(!"if the binary was not invoked manually, this is definitely a bug.");
            exit(1);
        }

        detail::trace_sync("branch.worker.wait_group", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " name=_sintra_all_processes";
        });
        m_group_all = Coordinator::rpc_wait_for_instance(s_coord_id, "_sintra_all_processes");
        detail::trace_sync("branch.worker.got_group", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " name=_sintra_all_processes";
        });
        detail::trace_sync("branch.worker.wait_group", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " name=_sintra_external_processes";
        });
        m_group_external = Coordinator::rpc_wait_for_instance(s_coord_id, "_sintra_external_processes");
        detail::trace_sync("branch.worker.got_group", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " name=_sintra_external_processes";
        });
    }

    // assign_name requires that all group processes are instantiated, in order
    // to receive the instance_published event
    if (s_recovery_occurrence == 0) {
        detail::trace_sync("branch.barrier.enter", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " group=_sintra_all_processes";
        });
        bool all_started = Process_group::rpc_barrier(m_group_all, UIBS);
        detail::trace_sync("branch.barrier.exit", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " group=_sintra_all_processes"
               << " success=" << all_started;
        });
        if (!all_started) {
            return false;
        }
    }

    return true;
}


inline
void Managed_process::go()
{
    assign_name(std::string("sintra_process_") + std::to_string(m_pid));

    m_entry_function();

    // Calling deactivate_all() is definitely wrong for the coordinator process.
    // For the rest, it is probably harmless.
    // The point of calling this here is to prevent running a handler
    // while the process is not in a state capable of running a handler (e.g. during destruction).
    // It is the responsibility of the library user to handle this situation properly, but
    // there might just be too many expectations from the user.
    if (!s_coord) {
        s_mproc->deactivate_all();
    }
}



 //////////////////////////////////////////////////////////////////////////
///// BEGIN START/STOP /////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//



inline
void Managed_process::pause()
{
    std::lock_guard<mutex> start_stop_lock(m_start_stop_mutex);

    // pause() might be called when the entry function finishes execution,
    // explicitly from one of the handlers, or from the entry function itself.
    // If called when the process is already paused, this should not have any
    // side effects.
    if (m_communication_state <= COMMUNICATION_PAUSED)
        return;

    {
        std::shared_lock<std::shared_mutex> readers_lock(m_readers_mutex);
        for (auto& ri : m_readers) {
            ri.second.pause();
        }
    }

    m_communication_state = COMMUNICATION_PAUSED;
    m_start_stop_condition.notify_all();
}


inline
void Managed_process::stop()
{
    std::lock_guard<mutex> start_stop_lock(m_start_stop_mutex);

    // stop() might be called explicitly from one of the handlers, or from the
    // entry function. If called when the process is already stopped, this
    // should not have any side effects.
    if (m_communication_state == COMMUNICATION_STOPPED)
        return;

    {
        std::shared_lock<std::shared_mutex> readers_lock(m_readers_mutex);
        for (auto& ri : m_readers) {
            ri.second.stop_nowait();
        }
    }

    m_communication_state = COMMUNICATION_STOPPED;
    m_start_stop_condition.notify_all();
}



inline
void Managed_process::wait_for_stop()
{
    std::unique_lock<mutex> start_stop_lock(m_start_stop_mutex);
    while (m_communication_state == COMMUNICATION_RUNNING) {
        m_start_stop_condition.wait(start_stop_lock);
    }
}


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END START/STOP ///////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////



inline
std::string Managed_process::obtain_swarm_directory()
{
    std::string sintra_directory = fs::temp_directory_path().string() + "/sintra/";
    if (!check_or_create_directory(sintra_directory)) {
        throw std::runtime_error("access to a working directory failed");
    }

    std::stringstream stream;
    stream << std::hex << m_swarm_id;
    auto swarm_directory = sintra_directory + stream.str();

    if (m_coordinator_is_local) {
        detail::trace_sync("swarm.prepare", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " path=" << swarm_directory;
        });
        std::error_code cleanup_ec;
        std::cerr << "[Managed_process] coordinator preparing swarm directory '"
                  << swarm_directory << "'" << std::endl;
        fs::remove_all(fs::path(swarm_directory), cleanup_ec);
        if (cleanup_ec) {
            std::ostringstream message;
            message << "failed to clear previous swarm directory '"
                    << swarm_directory << "': " << cleanup_ec.message();
            throw std::runtime_error(message.str());
        }
        std::cerr << "[Managed_process] coordinator cleared swarm directory"
                  << std::endl;
        detail::trace_sync("swarm.cleared", [&](auto& os) {
            os << "instance=" << m_instance_id
               << " swarm=" << m_swarm_id
               << " path=" << swarm_directory;
        });
    }

    if (!check_or_create_directory(swarm_directory)) {
        throw std::runtime_error("access to a working directory failed");
    }

    std::cerr << "[Managed_process] process " << m_instance_id
              << " using swarm directory '" << swarm_directory << "'"
              << std::endl;
    detail::trace_sync("swarm.using", [&](auto& os) {
        os << "instance=" << m_instance_id
           << " swarm=" << m_swarm_id
           << " path=" << swarm_directory;
    });
    return swarm_directory;
}



// Calls f when the specified transceiver becomes available.
// if the transceiver is available, f is invoked immediately.
template <typename T>
function<void()> Managed_process::call_on_availability(Named_instance<T> transceiver, function<void()> f)
{
    lock_guard<mutex> lock(m_availability_mutex);

    auto iid = Typed_instance_id<T>(get_instance_id(std::move(transceiver)));

    //if the transceiver is available, call f and skip the queue
    if (iid.id != invalid_instance_id) {
        f();

        // it's done - there is nothing to disable, thus returning an empty function.
        return []() {};
    }

    tn_type tn = { get_type_id<T>(), transceiver };

    // insert an empty function, in order to be able to capture the iterator within it
    auto& call_list = m_queued_availability_calls[tn];
    call_list.emplace_back();
    auto f_it = std::prev(call_list.end());

    struct availability_call_state {
        bool active = true;
        decltype(call_list.begin()) iterator;
    };

    auto state = std::make_shared<availability_call_state>();
    state->iterator = f_it;

    auto mark_completed = [this, tn, state](bool erase_empty_entry) -> bool {
        if (!state->active) {
            state->iterator = decltype(state->iterator){};
            return false;
        }

        auto queue_it = m_queued_availability_calls.find(tn);
        if (queue_it == m_queued_availability_calls.end()) {
            state->active = false;
            state->iterator = decltype(state->iterator){};
            return false;
        }

        auto call_it = state->iterator;
        queue_it->second.erase(call_it);
        if (erase_empty_entry && queue_it->second.empty()) {
            m_queued_availability_calls.erase(queue_it);
        }
        state->active = false;
        state->iterator = decltype(state->iterator){};
        return true;
    };

    // this is the abort call
    auto ret = Adaptive_function([this, state, mark_completed]() {
        std::lock_guard<std::mutex> lock(m_availability_mutex);
        mark_completed(true);
    });

    // and this is the actual call, which besides calling f, also neutralizes the
    // returned abort calls and marks this entry as completed.
    *f_it = [this, f, ret, mark_completed]() mutable {
        bool completed = false;
        {
            std::lock_guard<std::mutex> lock(m_availability_mutex);
            completed = mark_completed(false);
        }
        // Release the mutex before invoking the callback so user code can
        // safely register additional availability handlers without
        // encountering recursive locking.
        if (!completed) {
            return;
        }

        ret.set([]() {});
        f();
    };

    return ret;
}


inline
void Managed_process::wait_until_all_external_readers_are_done(int extra_allowed_readers)
{
    unique_lock<mutex> lock(m_num_active_readers_mutex);
    while (m_num_active_readers > 2 + extra_allowed_readers) {
        m_num_active_readers_condition.wait(lock);
    }
}


inline void Managed_process::unpublish_all_transceivers()
{
    std::vector<Transceiver*> to_destroy;
    to_destroy.reserve(m_local_pointer_of_instance_id.size());

    for (auto& entry : m_local_pointer_of_instance_id) {
        auto iid = entry.first;
        auto* transceiver = entry.second;
        if (!transceiver || iid == m_instance_id) {
            continue;
        }

        to_destroy.push_back(transceiver);
    }

    for (auto* transceiver : to_destroy) {
        if (transceiver) {
            transceiver->destroy();
        }
    }
}

inline void Managed_process::flush(instance_id_type process_id, sequence_counter_type flush_sequence)
{
    assert(is_process(process_id));

    Process_message_reader* reader = nullptr;
    sequence_counter_type rs = invalid_sequence;
    {
        std::shared_lock<std::shared_mutex> readers_lock(m_readers_mutex);
        auto it = m_readers.find(process_id);
        if (it == m_readers.end()) {
            throw std::logic_error(
                "attempted to flush the channel of a process which is not being read"
            );
        }

        // Barrier completion messages are RPC responses sent on the reply ring.
        // Check the reply reading sequence, not the request reading sequence.
        reader = &it->second;
        rs = reader->get_reply_reading_sequence();
    }

    if (rs >= flush_sequence) {
        return;
    }

    std::unique_lock<mutex> flush_lock(m_flush_sequence_mutex);
    m_flush_sequence.push_back(flush_sequence);

    while (reader->get_reply_reading_sequence() < flush_sequence &&
           m_communication_state == COMMUNICATION_RUNNING)
    {
        m_flush_sequence_condition.wait_for(
            flush_lock,
            std::chrono::milliseconds(500));
    }

    while (!m_flush_sequence.empty() &&
           m_flush_sequence.front() <= flush_sequence)
    {
        m_flush_sequence.pop_front();
    }
}


inline void Managed_process::run_after_current_handler(function<void()> task)
{
    if (!task) {
        return;
    }

    if (!tl_is_req_thread) {
        task();
        return;
    }

    if (!tl_post_handler_function) {
        tl_post_handler_function = std::move(task);
        return;
    }

    auto previous = std::move(tl_post_handler_function);
    tl_post_handler_function = [prev = std::move(previous), task = std::move(task)]() mutable {
        if (prev) {
            prev();
        }
        task();
    };
}


inline
void Managed_process::wait_for_delivery_fence()
{
    std::vector<Process_message_reader::Delivery_target> targets;

    {
        std::shared_lock<std::shared_mutex> readers_lock(m_readers_mutex);
        targets.reserve(m_readers.size() * 2);

        for (auto& [process_id, reader] : m_readers) {
            (void)process_id;
            if (reader.state() != Process_message_reader::READER_NORMAL) {
                continue;
            }

            const auto req_target = reader.get_request_leading_sequence();
            auto req_target_info = reader.prepare_delivery_target(
                Process_message_reader::Delivery_stream::Request,
                req_target);
            if (req_target_info.wait_needed) {
                targets.emplace_back(std::move(req_target_info));
            }

            const auto rep_target = reader.get_reply_leading_sequence();
            auto rep_target_info = reader.prepare_delivery_target(
                Process_message_reader::Delivery_stream::Reply,
                rep_target);
            if (rep_target_info.wait_needed) {
                targets.emplace_back(std::move(rep_target_info));
            }
        }
    }

    if (targets.empty()) {
        return;
    }

    auto all_targets_satisfied = [&]() {
        for (const auto& target : targets) {
            auto progress = target.progress.lock();
            if (!progress) {
                // Reader was replaced or destroyed; treat as satisfied because
                // no further progress is possible on the captured stream.
                continue;
            }

            const auto observed = (target.stream == Process_message_reader::Delivery_stream::Request)
                ? progress->request_sequence.load(std::memory_order_acquire)
                : progress->reply_sequence.load(std::memory_order_acquire);

            if (observed >= target.target) {
                continue;
            }

            const auto stopped = (target.stream == Process_message_reader::Delivery_stream::Request)
                ? progress->request_stopped.load(std::memory_order_acquire)
                : progress->reply_stopped.load(std::memory_order_acquire);

            if (stopped) {
                continue;
            }

            return false;
        }

        return true;
    };

    if (all_targets_satisfied()) {
        return;
    }

    std::unique_lock<std::mutex> lk(m_delivery_mutex);
    m_delivery_condition.wait(lk, all_targets_satisfied);
}


inline void Managed_process::notify_delivery_progress()
{
    m_delivery_condition.notify_all();
}


inline
size_t Managed_process::unblock_rpc(instance_id_type process_instance_id)
{
    assert(!process_instance_id || is_process(process_instance_id));
    size_t ret = 0;
    unique_lock<mutex> ol(s_outstanding_rpcs_mutex());
    if (!s_outstanding_rpcs().empty()) {

        for (auto& c : s_outstanding_rpcs()) {
            unique_lock<mutex> il(c->keep_waiting_mutex);

            if (process_instance_id != invalid_instance_id &&
                process_of(c->remote_instance) == process_instance_id)
            {
                c->success = false;
                c->keep_waiting = false;
                c->cancelled = true;
                c->keep_waiting_condition.notify_one();
                ret++;
            }
        }
    }
    return ret;
}


} // sintra


#endif






