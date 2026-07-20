// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../logging.h"
#include "../process/coordinator.h"
#include "../process/managed_process.h"
#include "../process/dispatch_wait_guard.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#if defined(SINTRA_ENABLE_TEST_HOOKS)
#include <exception>
#endif
#include <functional>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>


namespace sintra {

using std::lock_guard;
using std::mutex;
using std::string;
using std::string_view;

namespace {

constexpr auto k_external_process_invitation_rejection_grace = std::chrono::seconds(2);
constexpr string_view k_processing_phase_barrier_prefix = "_sintra_processing_phase/";
constexpr string_view k_reserved_barrier_prefix = "_sintra_";

inline bool is_lifecycle_internal_barrier_name(const string& barrier_name)
{
    string_view base_name{barrier_name};
    if (base_name.starts_with(k_processing_phase_barrier_prefix)) {
        base_name.remove_prefix(k_processing_phase_barrier_prefix.size());
    }
    return base_name.starts_with(k_reserved_barrier_prefix);
}

inline void emit_direct_publish_waiters(
    instance_id_type   instance_id,
    const Coordinator* coordinator)
{
    if (s_tl_common_function_iid == invalid_instance_id) {
        return;
    }

    // publish_process_group() calls publish_transceiver() directly, outside an
    // RPC handler return path. Complete wait_for_instance() deferrals here.
    using return_message_type =
        Message<Enclosure<instance_id_type>, void, not_defined_type_id>;

    const auto common_function_iid = s_tl_common_function_iid;
    std::vector<instance_id_type> waiters(
        s_tl_additional_piids,
        s_tl_additional_piids + s_tl_additional_piids_size);

    s_tl_common_function_iid   = invalid_instance_id;
    s_tl_additional_piids_size = 0;

    for (const auto waiter : waiters) {
        auto* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(
            vb_size<return_message_type>(instance_id),
            instance_id);
        Transceiver::finalize_rpc_write(
            placed_msg,
            waiter,
            common_function_iid,
            coordinator,
            not_defined_type_id);
    }
}

} // namespace

namespace detail { namespace test_hooks {

// Stage names shared between the coordinator lock-stage call sites and the
// tests that rendezvous on them, so neither side can silently drift.
inline constexpr const char* k_stage_unpublish_pre_barrier_collection =
    "unpublish_transceiver/pre_barrier_completion_collection";
inline constexpr const char* k_stage_make_process_group_groups_locked =
    "make_process_group/groups_locked";
inline constexpr const char* k_stage_reserve_external_invitation_entered =
    "reserve_external_process_invitation/entered";
inline constexpr const char* k_stage_publish_transceiver_locked =
    "publish_transceiver/publish_locked";
inline constexpr const char* k_stage_managed_child_publication_identity_captured =
    "publish_transceiver/managed_child_identity_captured";

#if defined(SINTRA_ENABLE_TEST_HOOKS)
// Coordinator lock-stage hook: tests running in the coordinator process may
// install a callback to rendezvous threads at named coordinator locking
// stages (see coordinator_lock_order_test). The callback runs on RPC/reader
// threads while coordinator mutexes may be held; it must not call back into
// the coordinator.
using Coordinator_lock_stage_callback = void (*)(const char* stage);
inline std::atomic<Coordinator_lock_stage_callback> s_coordinator_lock_stage{nullptr};

// Runs inside the real begin_process_draining RPC handler before production
// drain state changes, so tests can force the transported result path.
using Coordinator_begin_process_draining_callback =
    void (*)(instance_id_type process_iid);
inline std::atomic<Coordinator_begin_process_draining_callback>
    s_coordinator_begin_process_draining{nullptr};

// Runs inside the real resolve_instance RPC handler so deadline tests can hold
// nested synchronous readiness work without replacing the RPC path.
using Coordinator_resolve_instance_callback = void (*)(const string& assigned_name);
inline std::atomic<Coordinator_resolve_instance_callback>
    s_coordinator_resolve_instance{nullptr};

// Observes the exact coordinator transition that removes a published
// transceiver's assigned-name mapping. The callback runs while the coordinator
// publish mutex is held and therefore must not call back into the coordinator.
using Coordinator_name_retired_callback =
    void (*)(instance_id_type instance_id, const string& assigned_name);
inline std::atomic<Coordinator_name_retired_callback>
    s_coordinator_name_retired{nullptr};

// Observes each still-present process-publication registry entry immediately
// before Coordinator destruction discards the registry as raw container state.
// The callback must not call back into the coordinator.
using Coordinator_destroying_publication_callback =
    void (*)(instance_id_type process_iid) noexcept;
inline std::atomic<Coordinator_destroying_publication_callback>
    s_coordinator_destroying_publication{nullptr};

using Recovery_decision_callback =
    void (*)(instance_id_type process_iid, bool scheduled) noexcept;
inline std::atomic<Recovery_decision_callback> s_recovery_decision{nullptr};

class Recovery_decision_for_test
{
public:
    explicit Recovery_decision_for_test(instance_id_type process_iid) noexcept
    :
        m_process_iid(process_iid),
        m_uncaught_exceptions(std::uncaught_exceptions())
    {}

    ~Recovery_decision_for_test() noexcept
    {
        if (std::uncaught_exceptions() != m_uncaught_exceptions) {
            return;
        }
        if (auto callback = s_recovery_decision.load(std::memory_order_acquire)) {
            callback(m_process_iid, m_scheduled);
        }
    }

    void mark_scheduled() noexcept { m_scheduled = true; }

private:
    instance_id_type m_process_iid;
    int              m_uncaught_exceptions;
    bool             m_scheduled = false;
};
#endif

}} // namespace detail::test_hooks

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline void coordinator_lock_stage_for_test(const char* stage)
{
    const auto callback =
        detail::test_hooks::s_coordinator_lock_stage.load(std::memory_order_acquire);
    if (callback) {
        callback(stage);
    }
}
#else
inline void coordinator_lock_stage_for_test(const char*) {}
#endif

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline void coordinator_name_retired_for_test(
    instance_id_type instance_id,
    const string&    assigned_name)
{
    const auto callback =
        detail::test_hooks::s_coordinator_name_retired.load(std::memory_order_acquire);
    if (callback) {
        callback(instance_id, assigned_name);
    }
}
#else
inline void coordinator_name_retired_for_test(instance_id_type, const string&) {}
#endif

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline void coordinator_resolve_instance_for_test(const string& assigned_name)
{
    const auto callback =
        detail::test_hooks::s_coordinator_resolve_instance.load(std::memory_order_acquire);
    if (callback) {
        callback(assigned_name);
    }
}
#else
inline void coordinator_resolve_instance_for_test(const string&) {}
#endif

// EXPORTED EXCLUSIVELY FOR RPC
inline
sequence_counter_type Process_group::barrier(
    const string&  barrier_name,
    int32_t        barrier_mode_tag)
{
    std::unique_lock basic_lock(m_call_mutex);
    instance_id_type caller_piid = s_tl_current_message->sender_instance_id;
    if (m_process_ids.find(caller_piid) == m_process_ids.end()) {
        throw std::logic_error("The caller is not a member of the process group.");
    }

    auto& barrier_entry = m_barriers[barrier_name];
    if (!barrier_entry) {
        barrier_entry = std::make_shared<Barrier>();
    }

    auto     barrier = barrier_entry; // keep the barrier alive even if the map rehashes
    Barrier& b       = *barrier;
    // Lock ordering: m_call_mutex before barrier->m to prevent deadlock.
    std::unique_lock<std::mutex> barrier_lock(b.m);

    // Atomically snapshot membership and filter draining processes while holding m_call_mutex.
    // This ensures a consistent view: no process can be added/removed or change draining state
    // between the membership snapshot and the draining filter.
    if (b.processes_pending.empty()) {
        // new or reused barrier (may have failed previously)
        b.processes_pending = m_process_ids;
        b.processes_arrived.clear();
        b.failed              = false;
        b.mode_tag            = barrier_mode_tag;
        b.common_function_iid = make_instance_id();

        // Filter out absent processes while still holding m_call_mutex for atomicity.
        if (auto* coord = s_coord) {
            const bool user_barrier = !is_lifecycle_internal_barrier_name(barrier_name);
            for (auto it = b.processes_pending.begin();
                it != b.processes_pending.end();)
            {
                if (coord->is_process_draining(*it) ||
                    (user_barrier && coord->is_process_in_collective_shutdown(*it)))
                {
                    it = b.processes_pending.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
    }
    else
    if (b.mode_tag != barrier_mode_tag) {
        throw std::logic_error(
            "Barrier mode mismatch for barrier '" + barrier_name +
            "'. All participants must use the same barrier mode.");
    }

    // Now safe to release m_call_mutex - barrier state is consistent and other threads
    // need to be able to arrive at the barrier concurrently
    basic_lock.unlock();

    b.processes_arrived.insert(caller_piid);
    b.processes_pending.erase(caller_piid);

    if (b.processes_pending.empty()) {
        // Last arrival
        auto additional_pids = b.processes_arrived;
        additional_pids.erase(caller_piid);

        assert(s_tl_common_function_iid == invalid_instance_id);
        assert(s_tl_additional_piids_size == 0);
        assert(additional_pids.size() < max_process_index);
        s_tl_additional_piids_size = 0;
        for (auto& e : additional_pids) {
            s_tl_additional_piids[s_tl_additional_piids_size++] = e;
        }

        const auto current_common_fiid = b.common_function_iid;
        s_tl_common_function_iid = current_common_fiid;
        // Release barrier->m before re-acquiring m_call_mutex, honoring the
        // m_call_mutex -> barrier->m lock order.
        barrier_lock.unlock();

        // Re-lock m_call_mutex to safely erase from m_barriers
        basic_lock.lock();
        auto it = m_barriers.find(barrier_name);
        if (it                              != m_barriers.end() &&
            it->second                                          &&
            it->second.get()                == barrier.get()    &&
            it->second->common_function_iid == current_common_fiid)
        {
            m_barriers.erase(it);
        }
        // basic_lock will unlock m_call_mutex on return
        // Use reply ring watermark (m_out_rep_c) since barrier completion messages
        // are sent on the reply channel. Get it at return time for the calling process.
        return s_mproc->m_out_rep_c->get_leading_sequence();
    }
    else {
        // Not last arrival - emit a deferral message now and return without a normal reply
        auto* current_message = s_tl_current_message;
        assert(current_message);

        deferral* placed_msg = s_mproc->m_out_rep_c->write<deferral>(
            vb_size<deferral>(b.common_function_iid),
            b.common_function_iid);
        Transceiver::finalize_rpc_write(
            placed_msg,
            current_message->sender_instance_id,
            current_message->function_instance_id,
            this,
            (type_id_type)detail::reserved_id::deferral);

        mark_rpc_reply_deferred();
        return 0;
    }
}


inline void Process_group::drop_from_inflight_barriers(
    instance_id_type                   process_iid,
    std::vector<Barrier_completion>&   completions,
    Barrier_scope                      scope)
{
    std::lock_guard basic_lock(m_call_mutex);

    for (auto barrier_it = m_barriers.begin(); barrier_it != m_barriers.end(); ) {
        if (scope == Barrier_scope::user_only &&
            is_lifecycle_internal_barrier_name(barrier_it->first))
        {
            ++barrier_it;
            continue;
        }

        auto barrier = barrier_it->second;
        if (!barrier) {
            barrier_it = m_barriers.erase(barrier_it);
            continue;
        }

        std::unique_lock barrier_lock(barrier->m);

        const bool touched_pending = barrier->processes_pending.erase(process_iid) > 0;
        const bool touched_arrived = barrier->processes_arrived.erase(process_iid) > 0;

        if (!touched_pending && !touched_arrived) {
            ++barrier_it;
            continue;
        }

        if (!barrier->processes_pending.empty()) {
            ++barrier_it;
            continue;
        }

        Barrier_completion completion;
        completion.common_function_iid = barrier->common_function_iid;
        completion.recipients.assign(
            barrier->processes_arrived.begin(),
            barrier->processes_arrived.end());

        if (touched_arrived) {
            completion.recipients.push_back(process_iid);
        }

        barrier->processes_arrived.clear();
        barrier->common_function_iid = invalid_instance_id;

        barrier_lock.unlock();

        barrier_it = m_barriers.erase(barrier_it);
        completions.push_back(std::move(completion));
    }
}

inline void Process_group::emit_barrier_completions(
    const std::vector<Barrier_completion>& completions)
{
    using return_message_type = Message<Enclosure<sequence_counter_type>, void, not_defined_type_id>;

    for (const auto& completion : completions) {
        if (completion.common_function_iid == invalid_instance_id) {
            continue;
        }

        if (completion.recipients.empty()) {
            continue;
        }

        // Get per-recipient flush token: compute it INSIDE the loop so each recipient
        // gets a watermark that's valid for their specific message write time.
        // This prevents hangs where a global token is ahead of some recipient's channel.
        for (auto recipient : completion.recipients) {
            const auto flush_sequence = s_mproc->m_out_rep_c->get_leading_sequence();

            auto* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(
                vb_size<return_message_type>(flush_sequence), flush_sequence);

            Transceiver::finalize_rpc_write(
                placed_msg,
                recipient,
                completion.common_function_iid,
                this,
                not_defined_type_id);
        }
    }
}


inline
Coordinator::Coordinator():
    Derived_transceiver<Coordinator>("", make_service_instance_id())
{
    // Pre-initialize/ all draining states to 0 (ACTIVE) so that concurrent reads from
    // barrier paths are safe without additional locking. The array is fixed-size and
    // never grows, eliminating data races from container mutation.
    for (auto& draining_state : m_draining_process_states) {
        draining_state = 0;
    }
    for (auto& collective_shutdown_state : m_collective_shutdown_process_states) {
        collective_shutdown_state = 0;
    }

    m_external_process_invitation_cleanup_thread = std::thread(
        [this] { external_process_invitation_cleanup_loop(); });
}



inline
Coordinator::~Coordinator()
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback =
            detail::test_hooks::s_coordinator_destroying_publication.load(
                std::memory_order_acquire))
    {
        for (const auto& [process_iid, registry] : m_transceiver_registry) {
            (void)registry;
            callback(process_iid);
        }
    }
#endif

    m_shutdown.store(true, std::memory_order_release);
    {
        std::lock_guard lock(m_external_process_invitations_mutex);
        m_external_process_invitation_cleanup_stop = true;
    }
    m_external_process_invitations_cv.notify_all();
    if (m_external_process_invitation_cleanup_thread.joinable()) {
        m_external_process_invitation_cleanup_thread.join();
    }

    {
        std::lock_guard<mutex> lock(m_recovery_threads_mutex);
        for (auto& thread : m_recovery_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
    s_coord     = nullptr;
    s_coord_id  = 0;
}

namespace detail {

inline bool external_attach_tokens_equal(const std::string& lhs, const std::string& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (size_t i = 0; i < lhs.size(); ++i) {
        diff |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
    }
    return diff == 0;
}

} // namespace detail

inline bool Coordinator::external_process_invitation_exists_unlocked(
    instance_id_type process_iid) const
{
    return m_external_process_invitations.find(process_iid) !=
        m_external_process_invitations.end();
}

inline bool Coordinator::external_process_invitation_exists(instance_id_type process_iid)
{
    std::lock_guard lock(m_external_process_invitations_mutex);
    return external_process_invitation_exists_unlocked(process_iid);
}

inline bool Coordinator::group_has_non_external_peer(
    const string&      group_name,
    instance_id_type   self_process_iid)
{
    std::vector<instance_id_type> members;
    {
        std::lock_guard groups_lock(m_groups_mutex);
        auto group_it = m_groups.find(group_name);
        if (group_it == m_groups.end()) {
            return false;
        }

        auto& group = group_it->second;
        std::lock_guard<mutex> group_lock(group.m_call_mutex);
        members.assign(group.m_process_ids.begin(), group.m_process_ids.end());
    }

    std::lock_guard publish_lock(m_publish_mutex);
    for (auto process_iid : members) {
        if (process_iid                                      != self_process_iid &&
            m_external_attached_processes.count(process_iid) == 0)
        {
            return true;
        }
    }

    return false;
}

inline bool Coordinator::process_id_is_known_for_external_invitation(
    instance_id_type process_iid)
{
    if (!detail::is_valid_process_instance_id(process_iid)) {
        return true;
    }

    if (process_iid == s_mproc_id || process_iid == process_of(s_coord_id)) {
        return true;
    }

    {
        std::lock_guard lock(m_publish_mutex);
        if (m_transceiver_registry.count(process_iid)  != 0) { return true; }
        if (m_joined_process_branch.count(process_iid) != 0) { return true; }
        for (const auto& entry : m_inflight_joins) {
            if (entry.second == process_iid) {
                return true;
            }
        }
    }
    if (s_mproc) {
        std::lock_guard<std::mutex> cache_lock(s_mproc->m_cached_spawns_mutex);
        if (s_mproc->m_cached_spawns.count(process_iid) != 0) {
            return true;
        }
    }

    {
        std::lock_guard lock(m_init_tracking_mutex);
        if (m_processes_in_initialization.count(process_iid) != 0) {
            return true;
        }
    }

    {
        std::lock_guard lock(m_groups_mutex);
        if (m_groups_of_process.count(process_iid) != 0) {
            return true;
        }
    }

    return s_mproc && s_mproc->has_process_reader(process_iid);
}

inline bool Coordinator::publish_process_group(Process_group& group, const string& name)
{
    group.initialize_type_id();
    if (publish_transceiver(group.m_type_id, group.m_instance_id, name) != group.m_instance_id) {
        return false;
    }

    emit_direct_publish_waiters(group.m_instance_id, this);
    group.m_published = true;
    return true;
}

inline void Coordinator::unpublish_process_group(Process_group& group)
{
    if (!group.m_published) {
        return;
    }

    (void)unpublish_transceiver(group.m_instance_id);
    group.m_published = false;
}

inline bool Coordinator::reserve_external_process_invitation(
    instance_id_type                       process_iid,
    const string&                          token,
    std::chrono::steady_clock::time_point  expires_at,
    uint32_t&                              occurrence_out)
{
    coordinator_lock_stage_for_test(
        detail::test_hooks::k_stage_reserve_external_invitation_entered);

    if (!s_mproc || token.empty() || expires_at <= std::chrono::steady_clock::now()) {
        return false;
    }

    std::lock_guard lock(m_external_process_invitations_mutex);
    if (external_process_invitation_exists_unlocked(process_iid)) {
        return false;
    }

    if (process_id_is_known_for_external_invitation(process_iid)) {
        return false;
    }

    auto& next_occurrence = m_external_process_invitation_next_occurrence[process_iid];
    if (next_occurrence == std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    ++next_occurrence;

    if (!s_mproc->prepare_process_reader(process_iid, next_occurrence, false)) {
        return false;
    }

    External_process_invitation_record record;
    record.token      = token;
    record.expires_at = expires_at;
    record.occurrence = next_occurrence;
    record.state      = External_process_invitation_state::pending;
    m_external_process_invitations.emplace(process_iid, std::move(record));
    occurrence_out = next_occurrence;
    m_external_process_invitations_cv.notify_all();
    return true;
}

inline bool Coordinator::cancel_external_process_invitation(instance_id_type process_iid)
{
    std::lock_guard lock(m_external_process_invitations_mutex);
    auto it = m_external_process_invitations.find(process_iid);
    if (it == m_external_process_invitations.end())                     { return false; }
    if (it->second.state != External_process_invitation_state::pending) { return false; }

    it->second.state      = External_process_invitation_state::rejecting;
    it->second.expires_at =
        std::chrono::steady_clock::now() + k_external_process_invitation_rejection_grace;
    m_external_process_invitations_cv.notify_all();
    return true;
}

inline bool Coordinator::cancel_external_process_invitation(
    instance_id_type   process_iid,
    const string&      token)
{
    std::lock_guard lock(m_external_process_invitations_mutex);
    auto it = m_external_process_invitations.find(process_iid);
    if (it == m_external_process_invitations.end())                                      { return false; }
    if (it->second.state != External_process_invitation_state::pending)                  { return false; }
    if (token.empty() || !detail::external_attach_tokens_equal(it->second.token, token)) { return false; }

    it->second.state      = External_process_invitation_state::rejecting;
    it->second.expires_at =
        std::chrono::steady_clock::now() + k_external_process_invitation_rejection_grace;
    m_external_process_invitations_cv.notify_all();
    return true;
}

inline void Coordinator::transition_expired_external_process_invitations(
    std::vector<instance_id_type>&         readers_to_remove,
    std::chrono::steady_clock::time_point& next_deadline)
{
    const auto now = std::chrono::steady_clock::now();
    next_deadline = std::chrono::steady_clock::time_point::max();

    for (auto it = m_external_process_invitations.begin();
        it != m_external_process_invitations.end();)
    {
        auto& record = it->second;
        if (record.expires_at <= now) {
            if (record.state == External_process_invitation_state::pending) {
                record.state      = External_process_invitation_state::rejecting;
                record.expires_at = now + k_external_process_invitation_rejection_grace;
                next_deadline     = std::min(next_deadline, record.expires_at);
                ++it;
                continue;
            }

            set_draining_state(it->first, 1);
            record.state = External_process_invitation_state::removing;
            readers_to_remove.push_back(it->first);
            ++it;
            continue;
        }

        next_deadline = std::min(next_deadline, record.expires_at);
        ++it;
    }
}

inline void Coordinator::remove_external_process_invitation_readers(
    const std::vector<instance_id_type>& process_ids)
{
    if (!s_mproc) {
        return;
    }

    for (auto process_iid : process_ids) {
        s_mproc->remove_process_reader(process_iid, 1.0);
    }

    if (!process_ids.empty()) {
        note_draining_state_change();
    }
}

inline void Coordinator::external_process_invitation_cleanup_loop()
{
    std::unique_lock lock(m_external_process_invitations_mutex);

    while (!m_external_process_invitation_cleanup_stop) {
        std::vector<instance_id_type> readers_to_remove;
        std::chrono::steady_clock::time_point next_deadline;
        transition_expired_external_process_invitations(readers_to_remove, next_deadline);

        if (!readers_to_remove.empty()) {
            lock.unlock();
            remove_external_process_invitation_readers(readers_to_remove);
            lock.lock();
            for (auto process_iid : readers_to_remove) {
                auto it = m_external_process_invitations.find(process_iid);
                if (it               != m_external_process_invitations.end() &&
                    it->second.state == External_process_invitation_state::removing)
                {
                    m_external_process_invitations.erase(it);
                }
            }
            m_external_process_invitations_cv.notify_all();
            continue;
        }

        if (m_external_process_invitation_cleanup_stop) {
            break;
        }

        if (m_external_process_invitations.empty()) {
            m_external_process_invitations_cv.wait(lock, [&] {
                return
                    m_external_process_invitation_cleanup_stop ||
                    !m_external_process_invitations.empty();
            });
        }
        else {
            m_external_process_invitations_cv.wait_until(lock, next_deadline);
        }
    }
}

inline void Coordinator::cancel_all_external_process_invitations()
{
    std::vector<instance_id_type> readers_to_remove;
    {
        std::lock_guard lock(m_external_process_invitations_mutex);
        readers_to_remove.reserve(m_external_process_invitations.size());
        for (const auto& entry : m_external_process_invitations) {
            set_draining_state(entry.first, 1);
            readers_to_remove.push_back(entry.first);
        }
        m_external_process_invitations.clear();
    }
    m_external_process_invitations_cv.notify_all();
    remove_external_process_invitation_readers(readers_to_remove);
}

inline bool Coordinator::add_external_process_to_standard_groups(instance_id_type process_iid)
{
    std::lock_guard lock(m_groups_mutex);

    struct group_update
    {
        string                          name;
        instance_id_type                group_iid = invalid_instance_id;
        unordered_set<instance_id_type> members;
        bool                            created = false;
    };

    std::vector<group_update> updates;

    auto erase_group_membership = [&](instance_id_type member_iid, instance_id_type group_iid) {
        auto process_groups_it = m_groups_of_process.find(member_iid);
        if (process_groups_it != m_groups_of_process.end()) {
            process_groups_it->second.erase(group_iid);
            if (process_groups_it->second.empty()) {
                m_groups_of_process.erase(process_groups_it);
            }
        }
    };

    auto rollback_updates = [&]() {
        for (auto update_it = updates.rbegin(); update_it != updates.rend(); ++update_it) {
            auto group_it = m_groups.find(update_it->name);
            if (group_it == m_groups.end()) {
                continue;
            }

            if (update_it->created) {
                unpublish_process_group(group_it->second);
                for (auto member_iid : update_it->members) {
                    erase_group_membership(member_iid, update_it->group_iid);
                }
                m_groups.erase(group_it);
            }
            else {
                group_it->second.remove_process(process_iid);
                erase_group_membership(process_iid, update_it->group_iid);
            }
        }
    };

    auto ensure_group = [&](const string& name, const unordered_set<instance_id_type>& members) {
        auto group_it = m_groups.find(name);
        if (group_it == m_groups.end()) {
            auto [new_it, inserted] = m_groups.try_emplace(name);
            (void)inserted;
            group_it = new_it;
            group_it->second.set(members);
            for (auto member_iid : members) {
                m_groups_of_process[member_iid].insert(group_it->second.m_instance_id);
            }
            updates.push_back({
                name,
                group_it->second.m_instance_id,
                members,
                true
            });
            if (!publish_process_group(group_it->second, name)) {
                return false;
            }
            return true;
        }

        group_it->second.add_process(process_iid);
        m_groups_of_process[process_iid].insert(group_it->second.m_instance_id);
        updates.push_back({
            name,
            group_it->second.m_instance_id,
            {process_iid},
            false
        });
        return true;
    };

    if (!ensure_group("_sintra_all_processes",      {s_mproc_id, process_iid}) ||
        !ensure_group("_sintra_external_processes", {process_iid}))
    {
        rollback_updates();
        return false;
    }

    return true;
}

inline bool Coordinator::claim_external_process_invitation(
    instance_id_type   process_iid,
    const string&      token)
{
    if (!detail::is_valid_process_instance_id(process_iid) || token.empty()) {
        return false;
    }

    if (m_shutdown.load(std::memory_order_acquire)) {
        return false;
    }

    if (!s_tl_current_message ||
        process_of(s_tl_current_message->sender_instance_id) != process_iid)
    {
        return false;
    }

    std::lock_guard<mutex> admission_lock(detail::s_teardown_admission_mutex);
    std::lock_guard lock(m_external_process_invitations_mutex);
    auto it = m_external_process_invitations.find(process_iid);
    if (it == m_external_process_invitations.end()) {
        return false;
    }

    auto&      record = it->second;
    const auto now    = std::chrono::steady_clock::now();
    if (record.state != External_process_invitation_state::pending) {
        return false;
    }
    if (record.expires_at <= now) {
        record.state      = External_process_invitation_state::rejecting;
        record.expires_at = now + k_external_process_invitation_rejection_grace;
        m_external_process_invitations_cv.notify_all();
        return false;
    }
    if (!detail::external_attach_tokens_equal(record.token, token)) {
        record.state      = External_process_invitation_state::rejecting;
        record.expires_at = now;
        m_external_process_invitations_cv.notify_all();
        return false;
    }

    if (s_tl_current_message->managed_child_custody_identity != 0 ||
        s_tl_current_message->managed_child_occurrence != record.occurrence)
    {
        return false;
    }

    if (detail::s_teardown_admission_closed.load(std::memory_order_acquire)) {
        return false;
    }

    if (!s_mproc) {
        return false;
    }
    {
        Dispatch_shared_lock readers_lock(s_mproc->m_readers_mutex);
        const auto reader = s_mproc->m_readers.find(process_iid);
        if (reader == s_mproc->m_readers.end() || !reader->second ||
            reader->second->get_occurrence() != record.occurrence ||
            reader->second->get_managed_child_custody_identity() != 0)
        {
            return false;
        }
    }
    {
        std::lock_guard publish_lock(m_publish_mutex);
        if (m_transceiver_registry.count(process_iid)  != 0 ||
            m_joined_process_branch.count(process_iid) != 0)
        {
            return false;
        }
        for (const auto& entry : m_inflight_joins) {
            if (entry.second == process_iid) {
                return false;
            }
        }
        if (s_mproc) {
            std::lock_guard<std::mutex> cache_lock(s_mproc->m_cached_spawns_mutex);
            if (s_mproc->m_cached_spawns.count(process_iid) != 0) {
                return false;
            }
        }
        const bool role_inserted = m_member_roles.emplace(
            process_iid,
            Member_role_record{
                Process_reader_identity{0, process_iid, record.occurrence},
                record.role}).second;
        if (!role_inserted) {
            return false;
        }
        m_transceiver_registry[process_iid];
        m_external_attached_processes[process_iid] = record.occurrence;
    }
    // The snapshot is taken on its own (the canonical order is m_publish_mutex
    // before m_init_tracking_mutex, see coordinator.h). Acting on the snapshot
    // after releasing the lock is safe because process ids are allocated
    // monotonically and never reused: a process id found in initialization
    // here cannot also complete a publish as this external process before the
    // rollback below runs.
    bool in_initialization = false;
    {
        std::lock_guard init_lock(m_init_tracking_mutex);
        in_initialization = m_processes_in_initialization.count(process_iid) != 0;
    }

    if (in_initialization) {
        std::lock_guard publish_lock(m_publish_mutex);
        m_member_roles.erase(process_iid);
        m_external_attached_processes.erase(process_iid);
        m_transceiver_registry.erase(process_iid);
        return false;
    }

    set_collective_shutdown_state(process_iid, 0);
    if (set_draining_state(process_iid, 0)) {
        note_draining_state_change();
    }

    if (!add_external_process_to_standard_groups(process_iid)) {
        std::lock_guard publish_lock(m_publish_mutex);
        m_member_roles.erase(process_iid);
        m_external_attached_processes.erase(process_iid);
        m_transceiver_registry.erase(process_iid);
        return false;
    }
    m_external_process_invitations.erase(it);
    m_external_process_invitations_cv.notify_all();
    return true;
}



// EXPORTED FOR RPC
inline
type_id_type Coordinator::resolve_type(const string& pretty_name)
{
    lock_guard<mutex> lock(m_type_resolution_mutex);
    // Hold spinlock while accessing the iterator to prevent use-after-invalidation
    {
        auto scoped_map = s_mproc->m_type_id_of_type_name.scoped();
        auto it         = scoped_map.get().find(pretty_name);
        if (it != scoped_map.get().end()) {
            return it->second;
        }
        // Spinlock released here automatically when scoped_map goes out of scope
    }

    // a type is always assumed to exist
    auto       scoped_map = s_mproc->m_type_id_of_type_name.scoped();
    const auto new_id     = make_type_id();
    scoped_map.get().emplace(pretty_name, new_id);
    return new_id;
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::resolve_instance(const string& assigned_name)
{
    coordinator_resolve_instance_for_test(assigned_name);

    // Hold spinlock while accessing the iterator to prevent use-after-invalidation
    auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
    auto it         = scoped_map.get().find(assigned_name);
    if (it != scoped_map.get().end()) {
        return it->second;
    }

    // unlike types, instances need explicit allocation
    return invalid_instance_id;
}



// EXPORTED EXCLUSIVELY FOR RPC
inline
instance_id_type Coordinator::wait_for_instance(const string& assigned_name)
{
    // This works similarly to a barrier. The difference is that
    // a barrier operates in a defined set of process instances, whereas
    // waiting on an instance is not restricted.
    // A caller waiting for an instance will be unblocked when the
    // instance is created and will not block at all if it exists already.
    // Waiting for an instance does not influence creation/deletion of
    // the instance, thus using it for synchronization may not always be
    // applicable.

    std::lock_guard publish_lock(m_publish_mutex);
    instance_id_type caller_piid = s_tl_current_message->sender_instance_id;

    auto iid = resolve_instance(assigned_name);
    if (iid != invalid_instance_id) {
        return iid;
    }

    auto& waited_info = m_instances_waited[assigned_name];
    waited_info.waiters.insert(caller_piid);

    instance_id_type common_function_iid = waited_info.common_function_iid;
    if (common_function_iid == invalid_instance_id) {
        common_function_iid = waited_info.common_function_iid = make_instance_id();
    }

    auto* current_message = s_tl_current_message;
    assert(current_message);

    deferral* placed_msg = s_mproc->m_out_rep_c->write<deferral>(
        vb_size<deferral>(common_function_iid),
        common_function_iid);
    Transceiver::finalize_rpc_write(
        placed_msg,
        current_message->sender_instance_id,
        current_message->function_instance_id,
        this,
        (type_id_type)detail::reserved_id::deferral);

    mark_rpc_reply_deferred();
    return invalid_instance_id;
}


inline instance_id_type Coordinator::resolve_managed_child_instance_locked(
    const string& assigned_name,
    uint64_t custody_identity,
    instance_id_type process_iid,
    uint32_t occurrence,
    const std::atomic<bool>& cancelled)
{
    if (cancelled.load(std::memory_order_acquire)) {
        return invalid_instance_id;
    }
    auto names = s_mproc->m_instance_id_of_assigned_name.scoped();
    const auto resolved = names.get().find(assigned_name);
    const auto iid = resolved != names.get().end()
        ? resolved->second
        : invalid_instance_id;
    if (iid == invalid_instance_id || process_of(iid) != process_iid) {
        return invalid_instance_id;
    }
    const auto process = m_transceiver_registry.find(process_iid);
    if (process == m_transceiver_registry.end()) {
        return invalid_instance_id;
    }
    const auto publication = process->second.find(iid);
    if (publication == process->second.end() ||
        !publication->second.reader_identity.matches(
            custody_identity,
            process_iid,
            occurrence))
    {
        return invalid_instance_id;
    }
    return iid;
}


inline instance_id_type Coordinator::resolve_managed_child_instance(
    const string& assigned_name,
    uint64_t custody_identity,
    instance_id_type process_iid,
    uint32_t occurrence,
    const std::atomic<bool>& cancelled)
{
    coordinator_resolve_instance_for_test(assigned_name);
    std::lock_guard publish_lock(m_publish_mutex);
    return resolve_managed_child_instance_locked(
        assigned_name,
        custody_identity,
        process_iid,
        occurrence,
        cancelled);
}


inline instance_id_type Coordinator::wait_for_managed_child_instance(
    const string& assigned_name,
    uint64_t custody_identity,
    instance_id_type process_iid,
    uint32_t occurrence,
    const std::atomic<bool>& cancelled)
{
    coordinator_resolve_instance_for_test(assigned_name);
    std::unique_lock publish_lock(m_publish_mutex);
    instance_id_type resolved = invalid_instance_id;
    m_managed_child_publication_changed.wait(publish_lock, [&]() {
        resolved = resolve_managed_child_instance_locked(
            assigned_name,
            custody_identity,
            process_iid,
            occurrence,
            cancelled);
        return resolved != invalid_instance_id ||
            cancelled.load(std::memory_order_acquire);
    });
    return resolved;
}


inline void Coordinator::notify_managed_child_readiness_cancelled()
{
    std::lock_guard publish_lock(m_publish_mutex);
    m_managed_child_publication_changed.notify_all();
}



// EXPORTED EXCLUSIVELY FOR RPC
inline
instance_id_type Coordinator::publish_transceiver(
    type_id_type       tid,
    instance_id_type   iid,
    const string&      assigned_name)
{
    const auto process_iid = process_of(iid);
    Process_reader_identity reader_identity;
    if (s_tl_current_message &&
        process_of(s_tl_current_message->sender_instance_id) == process_iid &&
        (s_tl_current_message->managed_child_custody_identity != 0 ||
         s_tl_current_message->managed_child_occurrence != 0))
    {
        reader_identity.custody_identity =
            s_tl_current_message->managed_child_custody_identity;
        reader_identity.process_iid = process_iid;
        reader_identity.occurrence =
            s_tl_current_message->managed_child_occurrence;
    }
    return publish_transceiver_with_reader_identity(
        tid,
        iid,
        assigned_name,
        reader_identity);
}


inline instance_id_type Coordinator::publish_transceiver_with_reader_identity(
    type_id_type                  tid,
    instance_id_type              iid,
    const string&                 assigned_name,
    const Process_reader_identity& reader_identity)
{
    coordinator_lock_stage_for_test(
        detail::test_hooks::k_stage_managed_child_publication_identity_captured);
    const auto process_iid = process_of(iid);

    std::lock_guard notification_lock(m_publication_notifications_mutex);
    std::lock_guard lock(m_publish_mutex);
    coordinator_lock_stage_for_test(
        detail::test_hooks::k_stage_publish_transceiver_locked);

    // empty strings are not valid names
    if (assigned_name.empty()) {
        return invalid_instance_id;
    }

    if (reader_identity.is_external_process()) {
        const auto attached = m_external_attached_processes.find(process_iid);
        if (attached == m_external_attached_processes.end() ||
            attached->second != reader_identity.occurrence)
        {
            return invalid_instance_id;
        }
    }

    auto pr_it = m_transceiver_registry.find(process_iid);
    auto entry = Transceiver_publication{
        tid,
        assigned_name,
        reader_identity};

    auto notify_publication_changed = [&]() {
        m_managed_child_publication_changed.notify_all();
    };

    auto record_member_role = [&]() {
        if (iid != process_iid || process_iid == s_mproc_id) {
            return true;
        }
        const auto existing = m_member_roles.find(process_iid);
        if (existing != m_member_roles.end()) {
            return existing->second.identity == reader_identity;
        }
        m_member_roles.emplace(
            process_iid,
            Member_role_record{
                reader_identity,
                detail::Member_lifetime_role::COORDINATOR_BOUND});
        return true;
    };

    auto true_sequence = [&](bool allow_notification_delay) {
        bool queued_notification = false;
        if (allow_notification_delay) {
            std::lock_guard init_lock(m_init_tracking_mutex);
            if (!m_processes_in_initialization.empty()) {
                // Delay instance_published while startup is still in progress to prevent
                // circular RPC activation between not-yet-ready processes.
                m_delayed_instance_publications.push_back(
                    Pending_instance_publication{tid, iid, assigned_name});
                queued_notification = true;
            }
        }

        if (!queued_notification) {
            emit_global<instance_published>(tid, iid, assigned_name);
        }
        assert(s_tl_additional_piids_size == 0);
        assert(s_tl_common_function_iid == invalid_instance_id);

        s_tl_additional_piids_size = 0;

        instance_id_type notified_common_fiid = invalid_instance_id;

        if (auto waited_node = m_instances_waited.extract(assigned_name)) {
            auto waited_info = std::move(waited_node.mapped());

            assert(waited_info.waiters.size() < max_process_index);
            for (auto& e : waited_info.waiters) {
                s_tl_additional_piids[s_tl_additional_piids_size++] = e;
            }

            notified_common_fiid = waited_info.common_function_iid;
        }

        s_tl_common_function_iid = notified_common_fiid;

        return iid;
    };

    if (pr_it != m_transceiver_registry.end()) { // the transceiver's process is known
        
        auto& pr = pr_it->second; // process registry

        // observe the limit of transceivers per process
        if (pr.size() >= max_public_transceivers_per_proc) {
            return invalid_instance_id;
        }

        // the transceiver must not have been already published
        if (pr.find(iid) != pr.end()) {
            return invalid_instance_id;
        }

        // the assigned_name should not be taken
        if (resolve_instance(assigned_name) != invalid_instance_id) {
            return invalid_instance_id;
        }
        if (!record_member_role()) {
            return invalid_instance_id;
        }

        s_mproc->m_instance_id_of_assigned_name.set_value(entry.name, iid);
        pr.emplace(iid, entry);
        notify_publication_changed();

        // Do NOT reset draining state here - only reset when publishing a NEW PROCESS (Managed_process),
        // not when publishing a regular transceiver. Resetting here could interfere with shutdown.

        return true_sequence(false);
    }
    else
    if (iid == process_iid) { // the transceiver is a Managed_process

        // the assigned_name should not be taken
        if (resolve_instance(assigned_name) != invalid_instance_id) {
            return invalid_instance_id;
        }
        if (!record_member_role()) {
            return invalid_instance_id;
        }

        s_mproc->m_instance_id_of_assigned_name.set_value(entry.name, iid);
        map<instance_id_type, Transceiver_publication> process_registry;
        process_registry.emplace(iid, entry);
        m_transceiver_registry.emplace(iid, std::move(process_registry));
        notify_publication_changed();

        // Reset draining state to 0 (ACTIVE) when publishing a Managed_process.
        // This handles recovery/restart scenarios where the process slot might still be marked as draining.
        set_collective_shutdown_state(process_iid, 0);
        set_draining_state(process_iid, 0);

        return true_sequence(true);
    }
    else {
        return invalid_instance_id;
    }
}



// EXPORTED FOR RPC
inline
bool Coordinator::unpublish_transceiver(instance_id_type iid)
{
    std::optional<Process_reader_identity> expected_identity;
    if (s_tl_current_message &&
        process_of(s_tl_current_message->sender_instance_id) == process_of(iid) &&
        (s_tl_current_message->managed_child_custody_identity != 0 ||
         s_tl_current_message->managed_child_occurrence != 0))
    {
        expected_identity.emplace();
        expected_identity->custody_identity =
            s_tl_current_message->managed_child_custody_identity;
        expected_identity->process_iid =
            process_of(s_tl_current_message->sender_instance_id);
        expected_identity->occurrence =
            s_tl_current_message->managed_child_occurrence;
    }
    return unpublish_transceiver_exact(iid, expected_identity, std::nullopt);
}

inline bool Coordinator::unpublish_transceiver_exact(
    instance_id_type iid,
    const std::optional<Process_reader_identity>& expected_identity,
    const std::optional<Crash_info>& crash_info)
{
    const auto process_iid = process_of(iid);
    if (crash_info &&
        (iid != process_iid || !expected_identity || !*expected_identity ||
         crash_info->process_iid != process_iid))
    {
        return false;
    }

    // Read the process publication identity before taking m_groups_mutex so
    // exact custody lookup never nests m_child_custody_mutex under either
    // coordinator lock. The publication is revalidated under m_publish_mutex
    // immediately before it is extracted below.
    Process_reader_identity process_publication_identity;
    Member_role_record      process_member_role;
    detail::Managed_child_occurrence_token custody_occurrence;
    std::shared_ptr<detail::Managed_child_custody_record> custody_lifetime;
    if (iid == process_iid) {
        {
            std::lock_guard publish_lock(m_publish_mutex);
            const auto process = m_transceiver_registry.find(process_iid);
            if (process == m_transceiver_registry.end()) {
                return false;
            }
            const auto publication = process->second.find(iid);
            if (publication == process->second.end()) {
                return false;
            }
            process_publication_identity =
                publication->second.reader_identity;
            if (expected_identity &&
                process_publication_identity != *expected_identity)
            {
                return false;
            }
            if (process_iid != s_mproc_id) {
                const auto role = m_member_roles.find(process_iid);
                if (role == m_member_roles.end() ||
                    role->second.identity != process_publication_identity)
                {
                    return false;
                }
                process_member_role = role->second;
            }
        }

        if (process_publication_identity.is_managed_child()) {
            if (!s_mproc) {
                return false;
            }
            custody_occurrence =
                s_mproc->child_custody_occurrence_token_exact(
                    process_publication_identity.custody_identity,
                    process_publication_identity.process_iid,
                    process_publication_identity.occurrence);
            if (!custody_occurrence) {
                return false;
            }
            custody_lifetime = custody_occurrence.custody.lock();
            if (!custody_lifetime) {
                return false;
            }
        }
    }

    std::unique_lock groups_lock(m_groups_mutex, std::defer_lock);
    if (iid == process_iid) {
        groups_lock.lock();
    }
    std::unique_lock notification_lock(m_publication_notifications_mutex);
    std::unique_lock publish_lock(m_publish_mutex);

    // the process of the transceiver must have been registered
    auto pr_it = m_transceiver_registry.find(process_iid);
    if (pr_it == m_transceiver_registry.end()) {
        return false;
    }

    auto& pr = pr_it->second; // process registry

    // the transceiver must have been published
    auto it = pr.find(iid);
    if (it == pr.end()) {
        return false;
    }

    const auto& stored_identity = it->second.reader_identity;
    if (expected_identity && stored_identity != *expected_identity) {
        return false;
    }
    if (iid == process_iid &&
        stored_identity != process_publication_identity)
    {
        return false;
    }
    if (iid == process_iid && process_iid != s_mproc_id) {
        const auto role = m_member_roles.find(process_iid);
        if (role == m_member_roles.end() ||
            role->second.identity != stored_identity ||
            role->second.identity != process_member_role.identity ||
            role->second.role != process_member_role.role)
        {
            return false;
        }
    }
    if (iid == process_iid && stored_identity.is_external_process()) {
        const auto attached = m_external_attached_processes.find(process_iid);
        if (attached == m_external_attached_processes.end() ||
            attached->second != stored_identity.occurrence)
        {
            return false;
        }
    }

    std::shared_ptr<Process_message_reader> retiring_reader;
    if (iid == process_iid && iid != s_mproc_id) {
        Dispatch_shared_lock readers_lock(s_mproc->m_readers_mutex);
        const auto reader = s_mproc->m_readers.find(process_iid);
        if (reader != s_mproc->m_readers.end()) {
            retiring_reader = reader->second;
            if (stored_identity &&
                (!retiring_reader ||
                 retiring_reader->get_process_instance_id() != process_iid ||
                 retiring_reader->get_occurrence() != stored_identity.occurrence ||
                 retiring_reader->get_managed_child_custody_identity() !=
                    stored_identity.custody_identity))
            {
                return false;
            }
        }
        else
        if (expected_identity && stored_identity.is_external_process()) {
            return false;
        }
    }

    // the transceiver is assumed to have a name, not an empty string
    // (it shouldn't have made it through publish_transceiver otherwise)
    assert(!it->second.name.empty());

    // delete the reverse name lookup entry
    {
        auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
        scoped_map.get().erase(it->second.name);
    }

    // Extract the complete publication record. Exact identity cannot outlive
    // or disappear independently from the publication it authorizes.
    auto publication = pr.extract(it);
    auto tn = std::move(publication.mapped());

    std::vector<Pending_instance_publication> ready_notifications;
    std::vector<Pending_completion>            pending_completions;
    uint64_t draining_index = 0;
    bool     was_draining   = false;

    // if it is a Managed_process is being unpublished, more cleanup is required
    if (iid == process_iid) {

        // Cleanup in-flight join tracking for this process/branch, if any.
        if (auto branch_it = m_joined_process_branch.find(process_iid); branch_it != m_joined_process_branch.end()) {
            m_inflight_joins.erase(branch_it->second);
            m_joined_process_branch.erase(branch_it);
        }

        ready_notifications = finalize_initialization_tracking(process_iid);
        if (!ready_notifications.empty()) {
            ready_notifications.erase(
                std::remove_if(
                    ready_notifications.begin(),
                    ready_notifications.end(),
                    [&](const Pending_instance_publication& publication) {
                        return process_of(publication.instance_id) == process_iid;
                    }),
                ready_notifications.end());
        }

        // remove all name lookup entries resolving to the unpublished process
        auto name_map = s_mproc->m_instance_id_of_assigned_name.scoped();
        for (auto name_it = name_map.begin(); name_it != name_map.end();)
        {
            if (process_of(name_it->second) == process_iid) {
                name_it = name_map.erase(name_it);
            }
            else {
                ++name_it;
            }
        }

        // remove the unpublished process's remaining registry entries
        m_transceiver_registry.erase(pr_it);
        m_external_attached_processes.erase(process_iid);
        m_member_roles.erase(process_iid);

        // CRITICAL: Mark as draining BEFORE removing from groups to prevent barriers
        // from including this process. This handles both graceful shutdown (where
        // begin_process_draining was already called) and crash scenarios (where it wasn't).
        draining_index = get_process_index(process_iid);
        exchange_draining_state(process_iid, 1, was_draining);

        coordinator_lock_stage_for_test(
            detail::test_hooks::k_stage_unpublish_pre_barrier_collection);

        pending_completions = collect_pending_barrier_completions_unlocked(
            process_iid, true);

        // Keep the draining bit set; it will be re-initialized to 0 (ACTIVE)
        // when a new process is published into this slot. This prevents the race
        // where resetting too early allows concurrent barriers to include a dying process.

    }

    publish_lock.unlock();

    // Publication notifications share the coordinator request-ring FIFO.
    // Enqueue them while the sequencing mutex still excludes a replacement,
    // but after releasing registry state.
    emit_global<instance_unpublished>(tn.type_id, iid, tn.name);
    for (auto& ready : ready_notifications) {
        emit_global<instance_published>(
            ready.type_id,
            ready.instance_id,
            ready.assigned_name);
    }
    notification_lock.unlock();
    if (groups_lock.owns_lock()) {
        groups_lock.unlock();
    }

    coordinator_name_retired_for_test(iid, tn.name);

    if (iid == process_iid) {
        if (!pending_completions.empty() && s_mproc) {
            s_mproc->run_after_current_handler([
                    this,
                    pending = std::move(pending_completions)
                ]() mutable
                {
                    emit_pending_barrier_completions(pending);
                });
        }

        if (s_mproc) {
            if (process_publication_identity.is_managed_child()) {
                s_mproc->note_child_initialization_complete(custody_occurrence);
                s_mproc->note_child_publication_retired(custody_occurrence);
            }
            s_mproc->breach_lifeline(process_iid);
        }
        if (retiring_reader) {
            if (process_publication_identity.is_external_process() && s_mproc) {
                s_mproc->retire_external_process_reader(retiring_reader);
            }
            else {
                retiring_reader->stop_nowait();
            }
        }
        note_draining_state_change();
        if (s_mproc && process_publication_identity.is_managed_child()) {
            // Publication retirement was committed by the registry transaction
            // above. Communication becomes terminal only after the captured
            // predecessor reader has stopped and outstanding RPC participation
            // has been unblocked. The occurrence token prevents a replacement
            // from satisfying its predecessor's fact.
            s_mproc->retire_child_communication(
                custody_occurrence,
                std::move(retiring_reader));
        }
    }

    std::optional<Crash_info> recovery_info;
    if (iid == process_iid) {
        process_lifecycle_event event;
        if (crash_info) {
            event.process_iid  = crash_info->process_iid;
            event.process_slot = crash_info->process_slot;
            event.status       = crash_info->status;
            event.why          = process_lifecycle_event::reason::crash;
            if (process_publication_identity.is_managed_child()) {
                recovery_info = crash_info;
            }
        }
        else {
            event.process_iid  = process_iid;
            event.process_slot = static_cast<uint32_t>(draining_index);
            event.status       = 0;
            event.why          = was_draining
                ? process_lifecycle_event::reason::normal_exit
                : process_lifecycle_event::reason::unpublished;
            if (!was_draining &&
                process_publication_identity.is_managed_child())
            {
                recovery_info = Crash_info{
                    process_iid,
                    static_cast<uint32_t>(draining_index),
                    0};
            }
        }
        emit_lifecycle_event(event);
    }

    if (recovery_info) {
        recover_if_required(*recovery_info, custody_occurrence);
    }

    return true;
}


inline sequence_counter_type Coordinator::begin_process_draining(instance_id_type process_iid)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const auto callback =
        detail::test_hooks::s_coordinator_begin_process_draining.load(
            std::memory_order_acquire);
    if (callback) {
        callback(process_iid);
    }
#endif

    set_draining_state(process_iid, 1);

    auto pending_completions = collect_pending_barrier_completions(
        process_iid,
        false);

    // Emit any barrier completions before reporting the reply-ring watermark for
    // this drain RPC. Callers use that watermark to flush the coordinator reply
    // channel, so it must include the completions that draining just triggered.
    emit_pending_barrier_completions(pending_completions);

    // Use reply ring watermark (m_out_rep_c) since barrier completion messages
    // are sent on the reply channel. Get it at return time for the draining process.
    auto watermark = s_mproc->m_out_rep_c->get_leading_sequence();

    note_draining_state_change();

    return watermark;
}


inline sequence_counter_type Coordinator::begin_collective_shutdown(instance_id_type process_iid)
{
    if (!detail::is_valid_process_instance_id(process_iid)) {
        return invalid_sequence;
    }

    const bool called_via_rpc =
        s_tl_current_message &&
        s_tl_current_message->receiver_instance_id == m_instance_id &&
        s_tl_current_message->message_type_id ==
            static_cast<type_id_type>(detail::reserved_id::begin_collective_shutdown);
    if (s_tl_current_message && !called_via_rpc) {
        return invalid_sequence;
    }

    if (called_via_rpc) {
        if (process_of(s_tl_current_message->sender_instance_id) != process_iid) {
            return invalid_sequence;
        }
    }
    else
    if (process_iid != s_mproc_id) {
        return invalid_sequence;
    }

    set_collective_shutdown_state(process_iid, 1);

    auto pending_completions = collect_pending_barrier_completions(
        process_iid,
        false,
        Barrier_scope::user_only);
    emit_pending_barrier_completions(pending_completions);

    return s_mproc ? s_mproc->m_out_rep_c->get_leading_sequence() : invalid_sequence;
}


inline bool Coordinator::is_process_draining(instance_id_type process_iid) const
{
    size_t draining_slot = 0;
    if (!draining_slot_of_index(get_process_index(process_iid), draining_slot)) {
        return false;
    }

    return m_draining_process_states[draining_slot].load() != 0;
}


inline bool Coordinator::is_process_in_collective_shutdown(instance_id_type process_iid) const
{
    size_t shutdown_slot = 0;
    if (!draining_slot_of_index(get_process_index(process_iid), shutdown_slot)) {
        return false;
    }

    return m_collective_shutdown_process_states[shutdown_slot].load() != 0;
}


inline void Coordinator::collect_known_process_candidates_unlocked(
    std::vector<instance_id_type>& candidates)
{
    candidates.clear();
    std::unordered_set<instance_id_type> external_invitation_processes;
    {
        std::lock_guard invitation_lock(m_external_process_invitations_mutex);
        external_invitation_processes.reserve(m_external_process_invitations.size());
        for (const auto& entry : m_external_process_invitations) {
            external_invitation_processes.insert(entry.first);
        }
    }

    // Snapshot known processes from the registry and initialization tracking.
    // Processes that have never registered any transceivers are not represented
    // here and therefore do not participate in coordinated draining.
    {
        std::lock_guard publish_lock(m_publish_mutex);
        for (const auto& entry : m_transceiver_registry) {
            candidates.push_back(entry.first);
        }
        for (const auto& entry : m_inflight_joins) {
            candidates.push_back(entry.second);
        }
    }

    {
        std::lock_guard init_lock(m_init_tracking_mutex);
        for (const auto& piid : m_processes_in_initialization) {
            candidates.push_back(piid);
        }
    }

    {
        std::lock_guard groups_lock(m_groups_mutex);
        auto it = m_groups.find("_sintra_all_processes");
        if (it != m_groups.end()) {
            auto& group = it->second;
            std::lock_guard<mutex> group_lock(group.m_call_mutex);
            for (const auto& piid : group.m_process_ids) {
                candidates.push_back(piid);
            }
        }
    }

    if (s_mproc) {
        Dispatch_shared_lock readers_lock(s_mproc->m_readers_mutex);
        for (const auto& entry : s_mproc->m_readers) {
            if (external_invitation_processes.count(entry.first) != 0) {
                continue;
            }
            candidates.push_back(entry.first);
        }
    }
}

inline bool Coordinator::all_known_processes_draining_unlocked(instance_id_type /*self_process*/)
{
    std::vector<instance_id_type> candidates;
    collect_known_process_candidates_unlocked(candidates);

    if (candidates.empty()) {
        return true;
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (auto piid : candidates) {
        // Process IDs in registry/initialization tracking must be valid (non-zero).
        if (piid == invalid_instance_id) {
            assert(false && "Invalid process ID in draining candidates");
            continue;
        }
        if (!is_process_draining(piid)) {
            return false;
        }
    }

    return true;
}

inline bool Coordinator::is_sole_known_process_unlocked(instance_id_type self_process)
{
    std::vector<instance_id_type> candidates;
    collect_known_process_candidates_unlocked(candidates);

    candidates.erase(
        std::remove(candidates.begin(), candidates.end(), invalid_instance_id),
        candidates.end());

    if (candidates.empty()) {
        return true;
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates.size() == 1 && candidates.front() == self_process;
}

inline bool Coordinator::is_sole_known_process(instance_id_type self_process)
{
    return is_sole_known_process_unlocked(self_process);
}


inline bool Coordinator::wait_for_all_draining(instance_id_type self_process)
{
    (void)self_process; // currently unused; kept for potential future refinements.

    std::unique_lock<std::mutex> lk(m_draining_state_mutex);
    assert(!m_waiting_for_all_draining.load(std::memory_order_acquire));
    m_waiting_for_all_draining.store(true, std::memory_order_release);

    const auto timeout     = m_drain_timeout;
    const bool has_timeout = (timeout.count() > 0);
    const auto deadline = has_timeout
        ? std::chrono::steady_clock::now() + timeout
        : std::chrono::steady_clock::time_point::max();

    bool all_drained = true;

    while (true) {
        const uint64_t observed_generation = m_draining_state_generation;
        lk.unlock();
        const bool drained_now = all_known_processes_draining_unlocked(self_process);
        lk.lock();

        if (drained_now) {
            break;
        }

        const auto generation_advanced = [&]() {
            return m_draining_state_generation != observed_generation;
        };

        bool woke_for_state_change = true;
        if (!generation_advanced()) {
            if (has_timeout) {
                woke_for_state_change =
                    m_all_draining_cv.wait_until(lk, deadline, generation_advanced);
            }
            else {
                m_all_draining_cv.wait(lk, generation_advanced);
            }
        }

        if (has_timeout && !woke_for_state_change) {
            m_waiting_for_all_draining.store(false, std::memory_order_release);
            lk.unlock();

            std::vector<instance_id_type> candidates;
            collect_known_process_candidates_unlocked(candidates);
            if (!candidates.empty()) {
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            }

            std::vector<instance_id_type> not_draining;
            not_draining.reserve(candidates.size());
            for (auto piid : candidates) {
                if (piid == invalid_instance_id) { continue;                     }
                if (!is_process_draining(piid))  { not_draining.push_back(piid); }
            }

            if (!not_draining.empty()) {
                auto ls = Log_stream(log_level::warning);
                ls << "Coordinator shutdown timed out waiting for draining processes:";
                for (auto piid : not_draining) {
                    ls << " " << static_cast<unsigned long long>(piid);
                }
                ls << "\n";
            }
            all_drained = false;
            return all_drained;
        }
    }

    m_waiting_for_all_draining.store(false, std::memory_order_release);
    return all_drained;
}


inline void Coordinator::note_draining_state_change()
{
    std::lock_guard<std::mutex> lk(m_draining_state_mutex);
    ++m_draining_state_generation;
    if (m_waiting_for_all_draining.load(std::memory_order_relaxed)) {
        m_all_draining_cv.notify_all();
    }
}


inline void Coordinator::set_drain_timeout(std::chrono::seconds timeout)
{
    std::lock_guard<std::mutex> lk(m_draining_state_mutex);
    m_drain_timeout = timeout;
}


inline void Coordinator::unpublish_transceiver_notify(instance_id_type transceiver_iid)
{
    unpublish_transceiver(transceiver_iid);
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::make_process_group(
    const string&                          name,
    const unordered_set<instance_id_type>& member_process_ids)
{
    std::lock_guard lock(m_groups_mutex);

    coordinator_lock_stage_for_test(
        detail::test_hooks::k_stage_make_process_group_groups_locked);

    // check if it exists
    if (m_groups.count(name)) {
        return invalid_instance_id;
    }

    auto [group_it, inserted] = m_groups.try_emplace(name);
    (void)inserted;
    group_it->second.set(member_process_ids);
    auto ret = group_it->second.m_instance_id;

    for (auto& e : member_process_ids) {
        m_groups_of_process[e].insert(ret);
    }

    if (!publish_process_group(group_it->second, name)) {
        for (auto& e : member_process_ids) {
            auto process_groups_it = m_groups_of_process.find(e);
            if (process_groups_it != m_groups_of_process.end()) {
                process_groups_it->second.erase(ret);
                if (process_groups_it->second.empty()) {
                    m_groups_of_process.erase(process_groups_it);
                }
            }
        }
        m_groups.erase(group_it);
        return invalid_instance_id;
    }

    return ret;
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::join_swarm(
    const string&  binary_name,
    int32_t        branch_index)
{
    if (!s_mproc || !s_coord) {
        return invalid_instance_id;
    }

    std::unique_lock<mutex> admission_lock(detail::s_teardown_admission_mutex);

    if (detail::s_teardown_admission_closed.load(std::memory_order_acquire)) {
        return invalid_instance_id;
    }

    if (branch_index < 1 || branch_index >= max_process_index) {
        return invalid_instance_id;
    }

    instance_id_type new_instance_id = invalid_instance_id;
    {
        std::lock_guard guard(m_publish_mutex);

        // Safety: refuse joins when the process space is nearly exhausted to avoid
        // runaway spawning that would otherwise trip hard asserts.
        // m_processes_in_initialization is guarded by m_init_tracking_mutex
        // (nested here, consistent with the m_publish_mutex ->
        // m_init_tracking_mutex order).
        size_t initializing = 0;
        {
            std::lock_guard init_lock(m_init_tracking_mutex);
            initializing = m_processes_in_initialization.size();
        }
        const auto current_processes = m_transceiver_registry.size();
        if (current_processes + initializing >= static_cast<size_t>(max_process_index)) {
            return invalid_instance_id;
        }

        // If a join for this branch is already underway, avoid spawning another
        // process and return the pending instance id instead.
        if (auto it = m_inflight_joins.find(branch_index); it != m_inflight_joins.end()) {
            return it->second;
        }

        new_instance_id = make_process_instance_id();
        m_inflight_joins.emplace(branch_index, new_instance_id);
        m_joined_process_branch.emplace(new_instance_id, branch_index);
    }

    // Add to groups BEFORE spawning so the process is already a group member
    // when it starts executing. This prevents a race where the spawned process
    // calls barrier() before being added to the group, causing it to be excluded
    // from the barrier's processes_pending set.
    {
        std::lock_guard groups_lock(m_groups_mutex);
        auto add_to_group = [&](const string& name) {
            auto it = m_groups.find(name);
            if (it != m_groups.end()) {
                it->second.add_process(new_instance_id);
                m_groups_of_process[new_instance_id].insert(it->second.m_instance_id);
            }
        };
        add_to_group("_sintra_all_processes");
        add_to_group("_sintra_external_processes");
    }

    Managed_process::Spawn_swarm_process_args spawn_args;
    spawn_args.binary_name = binary_name.empty() ? s_mproc->m_binary_name : binary_name;
    spawn_args.piid        = new_instance_id;
    spawn_args.occurrence  = 0;
    spawn_args.args        = {
        spawn_args.binary_name,
        "--branch_index",   std::to_string(branch_index),
        "--swarm_id",       std::to_string(s_mproc->m_swarm_id),
        "--instance_id",    std::to_string(new_instance_id),
        "--coordinator_id", std::to_string(s_coord_id),
        detail::k_skip_startup_barrier_arg
    };
    spawn_args.custody = s_mproc->accept_child_custody();
    detail::Managed_child_launch_attempt launch_attempt;
    Managed_process::Spawn_result result;
    try {
        launch_attempt = s_mproc->admit_child_custody_occurrence(
            spawn_args.custody,
            spawn_args.piid,
            spawn_args.occurrence);
        if (launch_attempt) {
            admission_lock.unlock();
            result = s_mproc->spawn_swarm_process(
                spawn_args, launch_attempt);
        }
        else {
            result.failure.kind = Managed_child_failure_kind::custody_closed;
            result.failure.occurrence = spawn_args.occurrence;
            result.failure.message = "Managed child custody is closed";
            s_mproc->note_child_custody_failure(
                spawn_args.custody, result.failure);
            admission_lock.unlock();
        }
    }
    catch (...) {
        s_mproc->request_child_custody_release(
            spawn_args.custody, detail::Release_mode::cleanup);
        throw;
    }

    if (!result.success) {
        detail::managed_child_post_spawn_for_test(
            detail::Managed_child_post_spawn_stage::join_no_child,
            spawn_args.piid,
            spawn_args.occurrence);
        s_mproc->request_child_custody_release(spawn_args.custody);
        // Roll back group insertion on spawn failure.
        std::lock_guard groups_lock(m_groups_mutex);
        auto remove_from_group = [&](const string& name) {
            auto it = m_groups.find(name);
            if (it != m_groups.end()) {
                it->second.remove_process(new_instance_id);
            }
        };
        remove_from_group("_sintra_all_processes");
        remove_from_group("_sintra_external_processes");
        m_groups_of_process.erase(new_instance_id);

        std::lock_guard guard(m_publish_mutex);
        m_inflight_joins.erase(branch_index);
        m_joined_process_branch.erase(new_instance_id);
        return invalid_instance_id;
    }

    return new_instance_id;
}



// EXPORTED FOR RPC
inline
void Coordinator::print(const string& str)
{
    Log_stream(log_level::info) << str;
}



// EXPORTED FOR RPC
inline
void Coordinator::enable_recovery(instance_id_type piid)
{
    assert(is_process(piid));
    bool externally_attached = false;
    {
        std::lock_guard publish_lock(m_publish_mutex);
        externally_attached = m_external_attached_processes.count(piid) != 0;
    }
    if (externally_attached) {
        Log_stream(log_level::warning)
            << "Recovery is not available for externally attached process "
            << static_cast<unsigned long long>(piid)
            << " because it has no Sintra-managed child custody or cached structured launch recipe.\n";
        return;
    }

    if (!s_mproc || !s_tl_current_message ||
        process_of(s_tl_current_message->sender_instance_id) != piid ||
        s_tl_current_message->managed_child_custody_identity == 0)
    {
        return;
    }

    const auto occurrence = s_mproc->child_custody_occurrence_token_exact(
        s_tl_current_message->managed_child_custody_identity,
        piid,
        s_tl_current_message->managed_child_occurrence);
    auto custody = occurrence.custody.lock();
    if (!custody) {
        return;
    }

    std::lock_guard<std::mutex> custody_lock(custody->mutex);
    if (!custody->release_state.open()) {
        return;
    }
    custody->recovery_requested = true;
}

inline
void Coordinator::recover_if_required(
    const Crash_info&                              info,
    const detail::Managed_child_occurrence_token& occurrence)
{
    assert(is_process(info.process_iid));

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    detail::test_hooks::Recovery_decision_for_test recovery_decision(
        info.process_iid);
#endif

    if (detail::s_teardown_admission_closed.load(std::memory_order_acquire)) {
        return;
    }

    if (!s_mproc || occurrence.process_instance_id != info.process_iid) {
        return;
    }

    auto custody = occurrence.custody.lock();
    if (!custody) {
        return;
    }

    Managed_process::Spawn_swarm_process_args spawn_args;
    bool recovery_recipe_available = false;
    {
        std::lock_guard<std::mutex> cache_lock(s_mproc->m_cached_spawns_mutex);
        const auto spawn_it = s_mproc->m_cached_spawns.find(info.process_iid);
        const auto next_occurrence =
            occurrence.occurrence == std::numeric_limits<uint32_t>::max()
                ? occurrence.occurrence
                : occurrence.occurrence + 1;
        if (spawn_it != s_mproc->m_cached_spawns.end() &&
            spawn_it->second.custody == custody &&
            spawn_it->second.occurrence == next_occurrence)
        {
            spawn_args = spawn_it->second;
            recovery_recipe_available = true;
        }
    }
    if (!recovery_recipe_available) {
        Log_stream(log_level::warning)
            << "Recovery skipped for process "
            << static_cast<unsigned long long>(info.process_iid)
            << " because its exact managed-child custody has no cached structured launch recipe.\n";
        return;
    }
    {
        std::lock_guard<std::mutex> custody_lock(custody->mutex);
        if (!custody->release_state.open() || !custody->recovery_requested ||
            !custody->find_occurrence_locked(
                occurrence.process_instance_id,
                occurrence.occurrence))
        {
            return;
        }
    }

    Recovery_policy policy;
    Recovery_runner runner;
    {
        std::lock_guard<mutex> lock(m_lifecycle_mutex);
        policy = m_recovery_policy;
        runner = m_recovery_runner;
    }

    bool should_recover = true;
    if (policy) {
        try {
            should_recover = policy(info);
        }
        catch (const std::exception& e) {
            Log_stream(log_level::warning)
                << "[sintra] Exception in recovery policy: " << e.what() << "\n";
        }
        catch (...) {
            Log_stream(log_level::warning)
                << "[sintra] Unknown exception in recovery policy.\n";
        }
    }

    if (!should_recover) {
        return;
    }

    auto should_cancel = [this]() {
        return
            m_shutdown.load(std::memory_order_acquire) ||
            detail::s_teardown_admission_closed.load(std::memory_order_acquire);
    };

    auto spawned = std::make_shared<std::atomic<bool>>(false);
    auto spawn_now = [
            this,
            custody,
            occurrence,
            should_cancel,
            spawn_args,
            spawned
        ]() mutable
    {
        std::unique_lock<mutex> admission_lock(
            detail::s_teardown_admission_mutex);
        if (should_cancel() || !s_mproc) {
            return;
        }
        {
            std::lock_guard<std::mutex> custody_lock(custody->mutex);
            if (!custody->release_state.open() ||
                !custody->recovery_requested ||
                !custody->find_occurrence_locked(
                    occurrence.process_instance_id,
                    occurrence.occurrence))
            {
                return;
            }
        }
        if (spawned->exchange(true)) {
            return;
        }
        if (spawn_args.occurrence == std::numeric_limits<uint32_t>::max()) {
            s_mproc->note_child_custody_failure(
                spawn_args.custody,
                {Managed_child_failure_kind::occurrence_admission,
                 spawn_args.occurrence,
                 0,
                 "Managed child occurrence counter exhausted"});
            return;
        }
        auto launch_attempt = s_mproc->admit_child_custody_occurrence(
            spawn_args.custody,
            spawn_args.piid,
            spawn_args.occurrence);
        if (!launch_attempt) {
            s_mproc->note_child_custody_failure(
                spawn_args.custody,
                {Managed_child_failure_kind::custody_closed,
                 spawn_args.occurrence,
                 0,
                 "Managed child custody is closed"});
            return;
        }
        admission_lock.unlock();
        s_mproc->spawn_swarm_process(
            spawn_args, launch_attempt);
    };

    if (!runner) {
        std::lock_guard<mutex> lock(m_recovery_threads_mutex);
        m_recovery_threads.emplace_back(detail::Exception_boundary{"recovery_runner"}.wrap(
            [spawn_now]() mutable { spawn_now(); }));
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        recovery_decision.mark_scheduled();
#endif
        return;
    }

    Recovery_control control;
    control.should_cancel = should_cancel;
    control.spawn = spawn_now;
    {
        std::lock_guard<mutex> lock(m_recovery_threads_mutex);
        m_recovery_threads.emplace_back(detail::Exception_boundary{"recovery_runner"}.wrap(
            [info, runner, control]() mutable { runner(info, control); }));
    }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    recovery_decision.mark_scheduled();
#endif
}

inline
void Coordinator::set_recovery_policy(Recovery_policy policy)
{
    std::lock_guard<mutex> lock(m_lifecycle_mutex);
    m_recovery_policy = std::move(policy);
}

inline
void Coordinator::set_recovery_runner(Recovery_runner runner)
{
    std::lock_guard<mutex> lock(m_lifecycle_mutex);
    m_recovery_runner = std::move(runner);
}

inline
void Coordinator::set_lifecycle_handler(Lifecycle_handler handler)
{
    std::lock_guard<mutex> lock(m_lifecycle_mutex);
    m_lifecycle_handler = std::move(handler);
}

inline
void Coordinator::begin_shutdown()
{
    m_shutdown.store(true, std::memory_order_release);
    cancel_all_external_process_invitations();

    bool changed = false;
    {
        std::lock_guard publish_lock(m_publish_mutex);
        for (const auto& attached : m_external_attached_processes) {
            if (set_draining_state(attached.first, 1)) {
                changed = true;
            }
        }
    }

    if (changed) {
        note_draining_state_change();
    }
}

inline
void Coordinator::emit_lifecycle_event(const process_lifecycle_event& event)
{
    Lifecycle_handler handler;
    {
        std::lock_guard<mutex> lock(m_lifecycle_mutex);
        handler = m_lifecycle_handler;
    }
    if (handler) {
        detail::Exception_boundary{"lifecycle_handler"}([&]{ handler(event); });
    }
}

inline
std::vector<Coordinator::Pending_instance_publication>
Coordinator::finalize_initialization_tracking(instance_id_type process_iid)
{
    std::vector<Pending_instance_publication> ready_notifications;
    {
        std::lock_guard lock(m_init_tracking_mutex);
        if (process_iid != invalid_instance_id) {
            m_processes_in_initialization.erase(process_iid);
        }
        if (m_processes_in_initialization.empty() && !m_delayed_instance_publications.empty()) {
            ready_notifications.swap(m_delayed_instance_publications);
        }
    }
    return ready_notifications;
}

inline
std::vector<Coordinator::Pending_completion>
Coordinator::collect_pending_barrier_completions_unlocked(
    instance_id_type   process_iid,
    bool               remove_process,
    Barrier_scope      scope)
{
    // Caller must hold m_groups_mutex.
    std::vector<Pending_completion> pending_completions;
    pending_completions.reserve(m_groups.size());

    for (auto& [name, group] : m_groups) {
        std::vector<Process_group::Barrier_completion> completions;
        group.drop_from_inflight_barriers(
            process_iid,
            completions,
            scope);
        if (!completions.empty()) {
            pending_completions.push_back({name, std::move(completions)});
        }
        if (remove_process) {
            group.remove_process(process_iid);
        }
    }

    if (remove_process) {
        m_groups_of_process.erase(process_iid);
    }

    return pending_completions;
}

inline
std::vector<Coordinator::Pending_completion>
Coordinator::collect_pending_barrier_completions(
    instance_id_type   process_iid,
    bool               remove_process,
    Barrier_scope      scope)
{
    std::lock_guard groups_lock(m_groups_mutex);
    return collect_pending_barrier_completions_unlocked(
        process_iid,
        remove_process,
        scope);
}

inline
void Coordinator::emit_pending_barrier_completions(
    const std::vector<Pending_completion>& pending_completions)
{
    if (pending_completions.empty() || !s_mproc) {
        return;
    }

    std::lock_guard groups_lock(m_groups_mutex);
    for (const auto& entry : pending_completions) {
        auto group_it = m_groups.find(entry.group_name);
        if (group_it != m_groups.end()) {
            group_it->second.emit_barrier_completions(entry.completions);
        }
    }
}

inline
bool Coordinator::draining_slot_of_index(uint64_t draining_index, size_t& slot) const
{
    if (draining_index == 0 || draining_index > static_cast<uint64_t>(max_process_index)) {
        return false;
    }

    slot = static_cast<size_t>(draining_index);
    return true;
}


inline
bool Coordinator::set_draining_state(instance_id_type process_iid, int value)
{
    size_t draining_slot = 0;
    if (!draining_slot_of_index(get_process_index(process_iid), draining_slot)) {
        return false;
    }
    m_draining_process_states[draining_slot] = value;
    return true;
}


inline
bool Coordinator::set_collective_shutdown_state(instance_id_type process_iid, int value)
{
    size_t shutdown_slot = 0;
    if (!draining_slot_of_index(get_process_index(process_iid), shutdown_slot)) {
        return false;
    }
    m_collective_shutdown_process_states[shutdown_slot] = value;
    return true;
}


inline
bool Coordinator::exchange_draining_state(
    instance_id_type   process_iid,
    int                value,
    bool&              was_draining)
{
    size_t draining_slot = 0;
    if (!draining_slot_of_index(get_process_index(process_iid), draining_slot)) {
        return false;
    }
    was_draining = (m_draining_process_states[draining_slot].load() != 0);
    m_draining_process_states[draining_slot] = value;
    return true;
}

inline
void Coordinator::mark_initialization_complete(instance_id_type process_iid)
{
    const auto custody_occurrence = s_mproc
        ? s_mproc->child_custody_occurrence_token(process_iid)
        : detail::Managed_child_occurrence_token{};
    {
        std::lock_guard notification_lock(m_publication_notifications_mutex);
        std::lock_guard publish_lock(m_publish_mutex);
        if (auto branch_it = m_joined_process_branch.find(process_iid); branch_it != m_joined_process_branch.end()) {
            m_inflight_joins.erase(branch_it->second);
            m_joined_process_branch.erase(branch_it);
        }
        auto ready_notifications = finalize_initialization_tracking(process_iid);
        for (auto& publication : ready_notifications) {
            emit_global<instance_published>(
                publication.type_id,
                publication.instance_id,
                publication.assigned_name);
        }
    }

    if (s_mproc) {
        s_mproc->note_child_initialization_complete(custody_occurrence);
    }
}



} // sintra
