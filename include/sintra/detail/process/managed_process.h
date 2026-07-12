// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../config.h"
#include "../globals.h"
#include "../ipc/rings.h"
#include "../messaging/message.h"
#include "../process/process_id.h"
#include "../messaging/process_message_reader.h"
#include "../resolve_type.h"
#include "../ipc/spinlocked_containers.h"
#include "../transceiver.h"
#include "../messaging/call_function_with_message_args.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#ifndef _WIN32
#include <sys/types.h>
#endif
#include <deque>
#include <float.h>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>



namespace sintra {

using std::atomic;
using std::condition_variable;
using std::deque;
using std::function;
using std::list;
using std::map;
using std::mutex;
using std::recursive_mutex;
using std::string;
using std::vector;


inline
string uibs_impl(const char *file, int line)
{
    return std::string(file) + ":" + std::to_string(line);
};

#define UIBS uibs_impl(__FILE__, __LINE__)



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
    string             m_binary_name;

    friend struct Managed_process;
};


struct Process_descriptor
{
    Entry_descriptor   entry;
    vector<string>     sintra_options;
    vector<string>     user_options;
    instance_id_type   assigned_instance_id = invalid_instance_id;

    Process_descriptor(
        const Entry_descriptor&    aentry,
        const vector<string>&      auser_options = vector<string>())
    :
        entry(aentry),
        user_options(auser_options)
    {
    }
    Process_descriptor(
        const string&              binary_path,
        const vector<string>&      auser_options = vector<string>())
    :
        entry(binary_path),
        user_options(auser_options)
    {
    }
    Process_descriptor(
        const char*                binary_path,
        const vector<string>&      auser_options = vector<string>())
    :
        entry(binary_path),
        user_options(auser_options)
    {
    }

    Process_descriptor(int(*entry_function)())
    :
        entry(entry_function)
    {
    }

    friend struct Managed_process;
};

struct Lifetime_policy
{
    bool               enable_lifeline      = true;
    int                hard_exit_code       = 99;
    int                hard_exit_timeout_ms = 100;
};

enum class Managed_child_failure_kind
{
    none,
    custody_not_accepted,
    custody_closed,
    occurrence_admission,
    reader_setup,
    lifeline_setup,
    native_spawn,
    native_identity,
    setup_exception,
    setup_worker_start,
    readiness_observation,
    release_worker_start,
    release_worker_execution
};

struct Managed_child_failure
{
    Managed_child_failure_kind kind = Managed_child_failure_kind::none;
    uint32_t                   occurrence = 0;
    int                        native_error = 0;
    std::string                message;
};


// Branch indices have the following meaning:
// -1: No branching has taken place - this variable is not relevant
//  0: The starter process, with the coordinator.
// >0: A spawned process.
inline int32_t s_branch_index = -1;


inline uint32_t s_recovery_occurrence = 0;


namespace detail {

inline constexpr const char* k_external_attach_token_arg      = "--external_attach_token";
inline constexpr const char* k_external_attach_occurrence_arg = "--external_attach_occurrence";
inline constexpr auto        k_external_attach_claim_timeout  = std::chrono::seconds(5);

class Managed_child_transport_retirement
{
public:
    bool publication_retired() const noexcept
    {
        return m_publication_retired;
    }

    bool retirement_started() const noexcept
    {
        return m_state == State::RETIRING;
    }

    bool retirement_terminal() const noexcept
    {
        return m_state == State::RETIRED;
    }

    bool ready_to_retire() const noexcept
    {
        return m_publication_retired && m_state == State::CAPTURED;
    }

    bool fully_retired() const noexcept
    {
        return m_publication_retired && m_state == State::RETIRED;
    }

    void note_publication_retired() noexcept
    {
        m_publication_retired = true;
    }

    bool capture_authority(
        const std::shared_ptr<Process_message_reader>& reader)
    {
        if (m_state == State::RETIRED) {
            return !reader && !m_reader;
        }
        if (m_state != State::UNCAPTURED) {
            return m_reader == reader;
        }
        m_reader = reader;
        m_state = State::CAPTURED;
        return true;
    }

    bool begin_retirement(std::shared_ptr<Process_message_reader>& reader)
    {
        if (m_state != State::CAPTURED) {
            return false;
        }
        m_state = State::RETIRING;
        reader = m_reader;
        return true;
    }

    bool reset_retirement() noexcept
    {
        if (m_state != State::RETIRING && m_state != State::CAPTURED) {
            return false;
        }
        m_state = State::CAPTURED;
        return true;
    }

    bool complete_retirement(
        const std::shared_ptr<Process_message_reader>& reader,
        std::shared_ptr<Process_message_reader>& retired_authority)
    {
        if (m_state == State::RETIRED) {
            return !reader && !m_reader;
        }
        if (m_state != State::RETIRING || m_reader != reader) {
            return false;
        }
        m_state = State::RETIRED;
        retired_authority = std::move(m_reader);
        return true;
    }

private:
    enum class State
    {
        UNCAPTURED,
        CAPTURED,
        RETIRING,
        RETIRED
    };

    bool                                    m_publication_retired = false;
    State                                   m_state = State::UNCAPTURED;
    std::shared_ptr<Process_message_reader> m_reader;
};

class Managed_child_native_authority
{
public:
    bool confirm_absent() const noexcept
    {
        return m_phase == Phase::ABSENT && m_pid < 0 &&
            !m_start_stamp_available && m_start_stamp == 0 &&
            !m_wait_status_available && m_wait_status == 0
#ifdef _WIN32
            && !m_process_handle_owned && m_process_handle == 0 &&
            !m_exit_observer_registered
#endif
            ;
    }

    bool commit_created(
        int pid,
        bool start_stamp_available,
        uint64_t start_stamp,
        bool already_exited,
        bool wait_status_available,
        int wait_status
#ifdef _WIN32
        , uintptr_t process_handle,
        bool process_handle_owned
#endif
        ) noexcept
    {
        if (!confirm_absent() ||
            (pid <= 0
#ifdef _WIN32
             && process_handle == 0
#endif
            ) ||
            (!already_exited && wait_status_available)
#ifdef _WIN32
            || process_handle_owned != (process_handle != 0)
#endif
            )
        {
            return false;
        }
        m_phase = already_exited ? Phase::EXITED : Phase::RUNNING;
        m_pid = pid;
        m_start_stamp_available = start_stamp_available;
        m_start_stamp = start_stamp;
        m_wait_status_available = wait_status_available;
        m_wait_status = wait_status;
#ifdef _WIN32
        m_process_handle = process_handle;
        m_process_handle_owned = process_handle_owned;
#endif
        return true;
    }

    bool note_exit(int wait_status, bool wait_status_available) noexcept
    {
        if (m_phase == Phase::ABSENT) {
            return false;
        }
        if (m_phase == Phase::EXITED) {
            return m_wait_status_available == wait_status_available &&
                (!wait_status_available || m_wait_status == wait_status);
        }
        m_phase = Phase::EXITED;
        m_wait_status_available = wait_status_available;
        m_wait_status = wait_status;
        return true;
    }

    bool created() const noexcept { return m_phase != Phase::ABSENT; }
    bool running() const noexcept { return m_phase == Phase::RUNNING; }
    bool exited() const noexcept { return m_phase == Phase::EXITED; }
    int pid() const noexcept { return m_pid; }
    bool start_stamp_available() const noexcept
    {
        return m_start_stamp_available;
    }
    uint64_t start_stamp() const noexcept { return m_start_stamp; }
    bool wait_status_available() const noexcept
    {
        return m_wait_status_available;
    }
    int wait_status() const noexcept { return m_wait_status; }

#ifdef _WIN32
    bool register_exit_observer() noexcept
    {
        if (!running() || !m_process_handle_owned ||
            m_process_handle == 0)
        {
            return false;
        }
        m_exit_observer_registered = true;
        return true;
    }

    void cancel_exit_observer() noexcept
    {
        m_exit_observer_registered = false;
    }

    bool take_owned_process_handle(
        uintptr_t expected_handle,
        uintptr_t& released_handle) noexcept
    {
        if (!m_process_handle_owned || m_process_handle == 0 ||
            m_process_handle != expected_handle)
        {
            return false;
        }
        released_handle = m_process_handle;
        m_process_handle = 0;
        m_process_handle_owned = false;
        return true;
    }

    bool process_handle_owned() const noexcept
    {
        return m_process_handle_owned;
    }
    uintptr_t process_handle() const noexcept { return m_process_handle; }
    bool exit_observer_registered() const noexcept
    {
        return m_exit_observer_registered;
    }
    bool fallback_wait_available() const noexcept
    {
        return running() && !m_exit_observer_registered &&
            m_process_handle_owned && m_process_handle != 0;
    }
#endif

private:
    enum class Phase
    {
        ABSENT,
        RUNNING,
        EXITED
    };

    Phase    m_phase = Phase::ABSENT;
    int      m_pid = -1;
    uint64_t m_start_stamp = 0;
    bool     m_start_stamp_available = false;
    int      m_wait_status = 0;
    bool     m_wait_status_available = false;
#ifdef _WIN32
    uintptr_t m_process_handle = 0;
    bool      m_process_handle_owned = false;
    bool      m_exit_observer_registered = false;
#endif
};

struct Managed_child_occurrence_record
{
    enum class setup_state {
        pending,
        no_child,
        ownership_ready
    };

    uint32_t                   occurrence = 0;
    instance_id_type           process_instance_id = invalid_instance_id;
    setup_state                setup = setup_state::pending;
    bool                       initialization_reservation_active = false;
    Managed_child_transport_retirement
                               transport;
    Managed_child_native_authority
                               native;
};

struct Managed_child_custody_record;

struct Managed_child_active_occurrence
{
    std::weak_ptr<Managed_child_custody_record> custody;
    uint32_t                                    occurrence = 0;
};

struct Managed_child_occurrence_token
{
    std::weak_ptr<Managed_child_custody_record> custody;
    instance_id_type                            process_instance_id = invalid_instance_id;
    uint32_t                                    occurrence = 0;

    explicit operator bool() const noexcept { return !custody.expired(); }
};

enum class Custody_phase
{
    open,
    releasing,
    released
};

enum class Readiness_phase
{
    not_requested,
    pending,
    reached,
    stopped
};

enum class Release_mode
{
    passive,
    cleanup
};

enum class Release_attempt_phase
{
    idle,
    running,
    failing,
    retryable
};

// One retained logical custody record.  Subsystems remain authoritative for
// their own facts; this record only joins their exact-occurrence reports.
struct Managed_child_custody_record
{
    mutable std::mutex                         mutex;
    std::condition_variable                    changed;
    uint64_t                                   identity = 0;
    Readiness_phase                            readiness = Readiness_phase::not_requested;
    Custody_phase                              phase = Custody_phase::open;
    std::atomic<bool>                          readiness_cancelled{false};
    Release_mode                               release_mode = Release_mode::passive;
    Release_attempt_phase                      release_attempt_phase =
        Release_attempt_phase::idle;
    uint64_t                                   release_attempt_generation = 0;
    Managed_child_failure                      last_failure;
    std::vector<Managed_child_occurrence_record> occurrences;
};

} // namespace detail



template <typename T>
sintra::type_id_type get_type_id();


template <typename = void>
sintra::instance_id_type get_instance_id(string&& assigned_name);


struct Managed_process: Derived_transceiver<Managed_process>
{
    Managed_process();
    ~Managed_process();


    void init(int argc, const char* const* argv);
    bool branch(vector<Process_descriptor>& branch_vector);
    void go();

    // Pauses the process. Once called, reader threads will continue running,
    // but in a mode where only messages originating from the coordinator are
    // processed.
    void pause();

    // Stops the readers and causes their threads to exit.
    void stop();


    void wait_for_stop();

    void enable_recovery();


    Message_ring_W*                     m_out_req_c = nullptr;
    Message_ring_W*                     m_out_rep_c = nullptr;

    spinlocked_umap<
        string,
        type_id_type
    >                                   m_type_id_of_type_name;

    spinlocked_umap<
        type_id_type,
        string
    >                                   m_type_name_of_explicit_id;

    spinlocked_umap<
        string,
        instance_id_type
    >                                   m_instance_id_of_assigned_name;

    spinlocked_umap<
        instance_id_type,
        Transceiver*
    >                                   m_local_pointer_of_instance_id;

    // START/STOP

    enum Communication_state
    {
        COMMUNICATION_STOPPED,  // ring threads are stopped
        COMMUNICATION_PAUSED,   // rings answer to SERVICE_MODE
        COMMUNICATION_RUNNING   // rings in NORMAL_MODE
    };

    Communication_state                 m_communication_state = COMMUNICATION_STOPPED;
    mutex                               m_start_stop_mutex;
    condition_variable                  m_start_stop_condition;


    Managed_process(Managed_process const&) = delete;
    Managed_process& operator=(Managed_process const&) = delete;


    string                              m_binary_name;

    detail::process_id_type             m_pid;
    uint64_t                            m_process_start_stamp = 0;

    atomic<bool>                        m_must_stop;
    condition_variable                  m_termination_condition;

    uint64_t                            m_swarm_id;
    string                              m_directory;

    inline
    string obtain_swarm_directory();

    function<int()>                     m_entry_function = [] { return 0; };

    sequence_counter_type               m_last_message_sequence;

    // don't be tempted to make this atomic and shared across processes, it's not faster.
    sequence_counter_type               m_check_sequence;

    double                              m_message_stats_reference_time;
    uint64_t                            m_messages_accepted_since_reference_time;
    uint64_t                            m_messages_rejected_since_reference_time;
    sequence_counter_type               m_total_sequences_missed;

    std::chrono::time_point<std::chrono::steady_clock>
                                        m_time_instantiated;

    map<
        instance_id_type,
        std::shared_ptr<Process_message_reader>
    >                                   m_readers;
    mutable std::shared_mutex           m_readers_mutex;
    mutable mutex                       m_delivery_mutex;
    condition_variable                  m_delivery_condition;

    int                                 m_num_active_readers = 0;
    mutex                               m_num_active_readers_mutex;
    condition_variable                  m_num_active_readers_condition;

    void wait_until_all_external_readers_are_done(
        int                extra_allowed_readers = 0);

    bool prepare_process_reader(
        instance_id_type   process_instance_id,
        uint32_t           occurrence,
        bool               replace_existing);

    void remove_process_reader(
        instance_id_type   process_instance_id,
        double             waiting_period = 1.0);

    bool has_process_reader(
        instance_id_type   process_instance_id) const;

    void flush(
        instance_id_type       process_id,
        sequence_counter_type  flush_sequence);

    void run_after_current_handler(
        function<void()>   task);

    void wait_for_delivery_fence();
    void notify_delivery_progress();


    size_t unblock_rpc(instance_id_type process_instance_id = invalid_instance_id);


    // This signal will be sent BEFORE the coordinator sends instance_unpublished
    // for this process. It is meant to notify crash guards about the reason of the
    // instance_unpublished event, which will follow shortly after.
    SINTRA_MESSAGE_RESERVED(terminated_abnormally, int status);
    SINTRA_MESSAGE_RESERVED(unpublish_transceiver_notify, instance_id_type transceiver_instance_id);

    unordered_map<Tn_type, list<function<void()>>>
                                        m_queued_availability_calls;

    // Guards access to queued availability callbacks.  The corresponding
    // implementation releases this lock before invoking user callbacks to
    // avoid reentrancy issues.
    mutex                               m_availability_mutex;

    // Named-instance activation can re-enter the activation path when
    // call_on_availability() invokes the callback inline for an already
    // published transceiver, so this registry lock must currently be recursive.
    recursive_mutex                     m_handlers_mutex;

    // Incremented before invoking copied local-event handlers outside
    // m_handlers_mutex, decremented after the dispatch window closes.
    // Individual slot state governs deactivation safety.
    std::atomic<uint32_t>               m_handlers_dispatch_depth{0};

    // Calls f when the specified transceiver becomes available.
    // if the transceiver is available, f is invoked immediately.
    template <typename T>
    function<void()> call_on_availability(Named_instance<T> transceiver, function<void()> f);

    void unpublish_all_transceivers();


    deque<sequence_counter_type>        m_flush_sequence;
    mutex                               m_flush_sequence_mutex;
    condition_variable                  m_flush_sequence_condition;

    // Guarded by m_handlers_mutex.
    handler_registry_type               m_active_handlers;

    // standard process groups
    instance_id_type                    m_group_all             = invalid_instance_id;
    instance_id_type                    m_group_external        = invalid_instance_id;

    // recovery
    bool                                m_recoverable           = false;
    std::string                         m_recovery_cmd;


    struct Spawn_swarm_process_args
    {
        std::string                binary_name;
        std::vector<std::string>   args;
        std::vector<std::string>   env_overrides;
        instance_id_type           piid;
        uint32_t                   occurrence = 0;
        Lifetime_policy            lifetime;
        std::shared_ptr<detail::Managed_child_custody_record> custody;
        bool                       custody_occurrence_admitted = false;
    };

    struct Spawn_result
    {
        bool                       success = false;
        bool                       os_process_created = false;
        bool                       os_process_already_reaped = false;
        bool                       os_wait_status_available = false;
        int                        os_wait_status = 0;
        bool                       lifeline_enabled = false;
        bool                       lifeline_write_retained = false;
        std::string                binary_name;
        instance_id_type           instance_id;
        int                        os_pid = -1;
#ifdef _WIN32
        uintptr_t                  os_process_handle = 0;
        bool                       os_process_handle_owned = false;
#endif
        Managed_child_failure      failure;
    };

#ifndef _WIN32
    struct Spawned_child_reap_slot
    {
        uint64_t reservation_id = 0;
        pid_t    pid = 0;
        uint64_t start_stamp = 0;
        bool     start_stamp_available = false;
        detail::Managed_child_occurrence_token occurrence;
    };
    std::vector<Spawned_child_reap_slot> m_spawned_child_pids;
    uint64_t                            m_next_spawned_child_reservation = 1;
    mutable std::mutex                  m_spawned_child_pids_mutex;

    void reap_finished_children();
#endif

    using lifeline_handle_type = uintptr_t;
    map<instance_id_type, lifeline_handle_type>
                                        m_lifeline_writes;
    mutable std::mutex                  m_lifeline_mutex;

    bool release_lifeline(instance_id_type process_instance_id);
    void release_all_lifelines();

    Spawn_result spawn_swarm_process(const Spawn_swarm_process_args& ssp_args);
    Spawn_result spawn_swarm_process_impl(const Spawn_swarm_process_args& ssp_args);

    std::shared_ptr<detail::Managed_child_custody_record> accept_child_custody();
    bool can_accept_child_custody(instance_id_type process_instance_id) const;
    bool admit_child_custody_occurrence(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        instance_id_type process_instance_id,
        uint32_t occurrence);
    void note_child_custody_failure(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        Managed_child_failure failure);
    void fail_release_attempt(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        uint64_t release_attempt_generation,
        Managed_child_failure failure,
        instance_id_type process_instance_id);
    void request_child_custody_release(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        detail::Release_mode release_mode = detail::Release_mode::passive);
    void request_all_child_custody_releases();
    bool wait_for_all_child_custodies(
        std::chrono::steady_clock::time_point deadline);
    bool all_child_custodies_released() const;
    bool child_custody_allows_recovery(instance_id_type process_instance_id) const;
    detail::Managed_child_occurrence_token child_custody_occurrence_token(
        instance_id_type process_instance_id) const;
    detail::Managed_child_occurrence_token child_custody_occurrence_token_exact(
        uint64_t          custody_identity,
        instance_id_type process_instance_id,
        uint32_t         occurrence) const;
    void note_child_initialization_complete(
        const detail::Managed_child_occurrence_token& token);
    void note_child_publication_retired(
        const detail::Managed_child_occurrence_token& token);
    bool capture_child_communication_retirement_authority(
        const detail::Managed_child_occurrence_token& token,
        const std::shared_ptr<Process_message_reader>& reader);
    bool capture_replaced_child_communication_authority(
        const std::shared_ptr<Process_message_reader>& reader);
    bool begin_child_communication_retirement(
        const detail::Managed_child_occurrence_token& token,
        uint64_t expected_release_attempt_generation,
        uint64_t& claimed_release_attempt_generation,
        std::shared_ptr<Process_message_reader>& reader);
    bool complete_child_communication_retirement(
        const detail::Managed_child_occurrence_token& token,
        const std::shared_ptr<Process_message_reader>& reader);
    void reset_child_communication_retirement(
        const detail::Managed_child_occurrence_token& token,
        uint64_t release_attempt_generation);
    bool join_child_communication(
        const detail::Managed_child_occurrence_token& token,
        const std::shared_ptr<Process_message_reader>& reader);
    void retire_child_communication(
        const detail::Managed_child_occurrence_token& token,
        std::shared_ptr<Process_message_reader> reader);
    void note_child_os_exit(
        const detail::Managed_child_occurrence_token& token,
        int wait_status,
        bool wait_status_available = true);
#ifndef _WIN32
    bool signal_child_native_exact(
        const detail::Managed_child_occurrence_token& token,
        int signal_number);
#endif
    bool wait_for_child_native_exit(
        const detail::Managed_child_occurrence_token& token,
        std::chrono::steady_clock::time_point deadline);
    bool cleanup_child_native(
        const detail::Managed_child_occurrence_token& token);
    void start_child_custody_worker(
        std::function<void()> worker,
        const char* failure_stage = nullptr,
        instance_id_type failure_process_instance_id = invalid_instance_id,
        uint32_t failure_occurrence = 0);
    void join_child_custody_workers();
    void retire_child_custody_if_complete(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody);

    map<instance_id_type, Spawn_swarm_process_args>
                                        m_cached_spawns;
    mutable std::mutex                  m_cached_spawns_mutex;

    mutable std::mutex                  m_child_custody_mutex;
    uint64_t                            m_next_child_custody_identity = 1;
    std::map<uint64_t, std::shared_ptr<detail::Managed_child_custody_record>>
                                        m_child_custodies;
    std::map<instance_id_type, detail::Managed_child_active_occurrence>
                                        m_child_custody_by_process;
    mutable std::mutex                  m_child_custody_workers_mutex;
    struct Child_custody_worker
    {
        std::thread                         thread;
        std::shared_ptr<std::atomic<bool>>  complete;
    };
    std::vector<Child_custody_worker>   m_child_custody_workers;
};


} // namespace sintra
