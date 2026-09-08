// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "managed_process.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace sintra {
namespace detail {

namespace test_hooks {
inline constexpr const char* k_managed_child_elevation_observer_start =
    "managed_child_elevation_observer_start";
}

struct Managed_child_native_elevation_state
{
    enum class Phase { RESERVED, BOUND, GRANTED, SETTLED };

    ~Managed_child_native_elevation_state()
    {
#ifdef _WIN32
        if (reference_handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(reference_handle));
        }
        if (broker_handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(broker_handle));
        }
#endif
    }

    std::shared_ptr<Managed_child_custody_record> custody;
    Managed_child_occurrence_identity            identity;
    uint64_t                                    generation = 0;
    std::chrono::steady_clock::time_point        deadline;
    uintptr_t                                   reference_handle = 0;
    uintptr_t                                   broker_handle = 0;
    uint32_t                                    target_pid = 0;
    Phase                                       phase = Phase::RESERVED;
    bool                                        broker_exited = false;
    bool                                        broker_identity_valid = false;
    bool                                        abandoned = false;
    Managed_child_native_error                   error;
    std::condition_variable                     binding_changed;
};

inline bool settle_native_elevation_locked(
    Managed_child_native_elevation_state& state,
    Managed_child_native_outcome outcome,
    Managed_child_native_error error)
{
    if (state.phase == Managed_child_native_elevation_state::Phase::SETTLED) {
        return true;
    }
    auto* occurrence = state.custody->find_occurrence_locked(
        state.identity.process_instance_id, state.identity.occurrence);
    if (!occurrence || occurrence->native_action.generation != state.generation) {
        state.error = {Managed_child_native_error_domain::PROVIDER, 0, "Native elevation action replaced"};
        return false;
    }
    if (occurrence->native.exited()) {
        outcome = Managed_child_native_outcome::EXITED;
        error = {};
    }
    else
    if (outcome == Managed_child_native_outcome::EXITED) {
        outcome = Managed_child_native_outcome::FAILED;
        error = {Managed_child_native_error_domain::PROVIDER, 0, "Native child exit not confirmed"};
    }
    state.phase = Managed_child_native_elevation_state::Phase::SETTLED;
    state.error = error;
    occurrence->native_action.stage   = Managed_child_native_stage::FINISHED;
    occurrence->native_action.outcome = outcome;
    occurrence->native_action.error   = std::move(error);
    return true;
}

inline Managed_child_native_error observe_elevated_child_exit(
    Managed_process& owner,
    const Managed_child_native_elevation_state& state)
{
#ifdef _WIN32
    HANDLE process = reinterpret_cast<HANDLE>(state.reference_handle);
    const DWORD wait = WaitForSingleObject(process, 0);
    if (wait != WAIT_OBJECT_0) {
        return wait == WAIT_FAILED
            ? Managed_child_native_error{Managed_child_native_error_domain::WINDOWS,
                  (int)GetLastError(), "WaitForSingleObject(elevation target)"}
            : Managed_child_native_error{};
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process, &exit_code)) {
        return {Managed_child_native_error_domain::WINDOWS, (int)GetLastError(),
            "GetExitCodeProcess(elevation target)"};
    }
    owner.note_child_os_exit(
        {state.custody, state.identity.process_instance_id, state.identity.occurrence},
        (int)exit_code);
#else
    (void)owner;
    (void)state;
#endif
    return {};
}

} // namespace detail

inline Managed_child_native_elevation_ticket::Managed_child_native_elevation_ticket(
    std::shared_ptr<detail::Managed_child_native_elevation_state> state)
:
    m_state(std::move(state))
{}

inline Managed_child_native_elevation_ticket::~Managed_child_native_elevation_ticket()
{
    if (m_state->generation == 0) {
        return;
    }
    using Phase = detail::Managed_child_native_elevation_state::Phase;
    {
        std::lock_guard<std::mutex> lock(m_state->custody->mutex);
        m_state->abandoned = true;
        if (m_state->phase == Phase::RESERVED || m_state->phase == Phase::SETTLED || m_state->broker_exited) {
            detail::settle_native_elevation_locked(*m_state,
                m_state->phase == Phase::RESERVED
                    ? Managed_child_native_outcome::CANCELLED
                    : Managed_child_native_outcome::FAILED,
                {Managed_child_native_error_domain::PROVIDER, 0, "Elevation ticket abandoned"});
        }
    }
    m_state->binding_changed.notify_all();
    m_state->custody->changed.notify_all();
}

inline Managed_child_occurrence_identity Managed_child_native_elevation_ticket::occurrence() const
{
    return m_state->identity;
}

inline uint64_t Managed_child_native_elevation_ticket::action_generation() const
{
    return m_state->generation;
}

inline Managed_child_native_error Managed_child_native_elevation_ticket::last_error() const
{
    std::lock_guard<std::mutex> lock(m_state->custody->mutex);
    return m_state->error;
}

inline managed_child_native_elevation_reference_t
Managed_child_native_elevation_ticket::export_reference() const
{
    std::lock_guard<std::mutex> lock(m_state->custody->mutex);
#ifdef _WIN32
    if (m_state->phase == detail::Managed_child_native_elevation_state::Phase::BOUND &&
        m_state->broker_identity_valid && std::chrono::steady_clock::now() < m_state->deadline)
    {
        return {GetCurrentProcessId(), m_state->reference_handle, m_state->target_pid};
    }
#endif
    m_state->error = {Managed_child_native_error_domain::PROVIDER, 0, "Elevation reference export unavailable"};
    return {};
}

inline bool Managed_child_native_elevation_ticket::bind_broker(uintptr_t broker_process_handle)
{
#ifdef _WIN32
    using Phase = detail::Managed_child_native_elevation_state::Phase;
    {
        std::lock_guard<std::mutex> lock(m_state->custody->mutex);
        if (m_state->phase != Phase::RESERVED) {
            m_state->error = {Managed_child_native_error_domain::PROVIDER, 0, "Elevation broker already bound or settled"};
            return false;
        }
        HANDLE broker = reinterpret_cast<HANDLE>(broker_process_handle);
        HANDLE retained = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), broker, GetCurrentProcess(), &retained,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
        {
            const auto error = GetLastError();
            m_state->error = {Managed_child_native_error_domain::WINDOWS, (int)error, "DuplicateHandle(broker)"};
            return false;
        }
        const DWORD pid = GetProcessId(retained);
        if (pid == 0 || pid == GetCurrentProcessId() || pid == m_state->target_pid) {
            const auto error = pid == 0 ? GetLastError() : ERROR_SUCCESS;
            CloseHandle(retained);
            m_state->error = {error != 0 ? Managed_child_native_error_domain::WINDOWS
                                        : Managed_child_native_error_domain::PROVIDER,
                (int)error, "Elevation broker must be a distinct process"};
            return false;
        }
        m_state->broker_handle = reinterpret_cast<uintptr_t>(retained);
        m_state->broker_identity_valid = true;
        m_state->error = {};
        m_state->phase = Phase::BOUND;
    }
    m_state->binding_changed.notify_all();
    return true;
#else
    (void)broker_process_handle;
    return false;
#endif
}

inline bool Managed_child_native_elevation_ticket::adopt_broker(uintptr_t owned_broker_process_handle)
{
#ifdef _WIN32
    using Phase = detail::Managed_child_native_elevation_state::Phase;
    {
        std::lock_guard<std::mutex> lock(m_state->custody->mutex);
        HANDLE broker = reinterpret_cast<HANDLE>(owned_broker_process_handle);
        if (m_state->phase != Phase::RESERVED || !broker || broker == INVALID_HANDLE_VALUE ||
            broker == GetCurrentThread())
        {
            m_state->error = {Managed_child_native_error_domain::PROVIDER, 0,
                "Elevation broker adoption requires an unbound ticket and an owned process handle"};
            return false;
        }
        const DWORD pid = GetProcessId(broker);
        const DWORD native_error = pid == 0 ? GetLastError() : ERROR_SUCCESS;
        const bool identity_valid = pid != 0 && pid != GetCurrentProcessId() && pid != m_state->target_pid;
        Managed_child_native_error validation_error;
        if (!identity_valid) {
            validation_error = {native_error != 0 ? Managed_child_native_error_domain::WINDOWS
                                                 : Managed_child_native_error_domain::PROVIDER,
                (int)native_error, "Elevation broker must be a distinct process"};
        }
        // Prepare diagnostic storage before transfer; thereafter only move
        // assignments publish it. Even rejected identity remains observed.
        auto action_error = validation_error;
        auto* occurrence = m_state->custody->find_occurrence_locked(
            m_state->identity.process_instance_id, m_state->identity.occurrence);
        m_state->broker_handle = owned_broker_process_handle;
        m_state->broker_identity_valid = identity_valid;
        m_state->phase = Phase::BOUND;
        m_state->error = std::move(validation_error);
        occurrence->native_action.error = std::move(action_error);
    }
    m_state->binding_changed.notify_all();
    m_state->custody->changed.notify_all();
    return true;
#else
    (void)owned_broker_process_handle;
    return false;
#endif
}

inline bool Managed_child_native_elevation_ticket::commit()
{
    using Phase = detail::Managed_child_native_elevation_state::Phase;
    bool committed = false;
    {
        std::lock_guard<std::mutex> lock(m_state->custody->mutex);
        auto* occurrence = m_state->custody->find_occurrence_locked(
            m_state->identity.process_instance_id, m_state->identity.occurrence);
        if (m_state->phase != Phase::BOUND || !m_state->broker_identity_valid ||
            std::chrono::steady_clock::now() >= m_state->deadline || occurrence->native_action.generation != m_state->generation || occurrence->native.exited())
        {
            m_state->error = {Managed_child_native_error_domain::PROVIDER, 0, "Elevation grant expired, settled or replaced"};
        }
#ifdef _WIN32
        else
        {
            const auto still_live = [this](uintptr_t handle, const char* operation) {
                const DWORD wait = WaitForSingleObject(reinterpret_cast<HANDLE>(handle), 0);
                if (wait == WAIT_TIMEOUT) {
                    return true;
                }
                const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
                m_state->error = {error != 0 ? Managed_child_native_error_domain::WINDOWS
                                           : Managed_child_native_error_domain::PROVIDER,
                    (int)error, operation};
                return false;
            };
            if (still_live(m_state->reference_handle, "WaitForSingleObject(elevation target)") &&
                still_live(m_state->broker_handle, "WaitForSingleObject(elevation broker)"))
            {
                m_state->phase = Phase::GRANTED;
                occurrence->native_action.stage = Managed_child_native_stage::ELEVATION_GRANTED;
                committed = true;
            }
        }
#endif
    }
    m_state->custody->changed.notify_all();
    return committed;
}

inline bool Managed_child_native_elevation_ticket::finish(
    Managed_child_native_outcome outcome,
    Managed_child_native_error error)
{
    using Phase = detail::Managed_child_native_elevation_state::Phase;
    Managed_child_native_error observation_error;
    const auto lifetime = m_state->custody->runtime_lifetime.lock();
    if (lifetime) {
        std::lock_guard<std::mutex> admission(lifetime->m_native_admission_mutex);
        if (lifetime->m_native_owner) {
            observation_error = detail::observe_elevated_child_exit(*lifetime->m_native_owner, *m_state);
        }
    }
    bool settled = false;
    {
        std::lock_guard<std::mutex> lock(m_state->custody->mutex);
#ifdef _WIN32
        if ((m_state->phase == Phase::BOUND || m_state->phase == Phase::GRANTED) && !m_state->broker_exited) {
            const DWORD wait = WaitForSingleObject(reinterpret_cast<HANDLE>(m_state->broker_handle), 0);
            m_state->broker_exited = wait == WAIT_OBJECT_0;
            if (!m_state->broker_exited) {
                const DWORD native_error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
                m_state->error = {native_error != 0 ? Managed_child_native_error_domain::WINDOWS
                                                  : Managed_child_native_error_domain::PROVIDER,
                    (int)native_error, "Elevation broker exit not confirmed"};
                return false;
            }
        }
#endif
        if (outcome == Managed_child_native_outcome::NONE || outcome == Managed_child_native_outcome::ACTIVE ||
            (outcome == Managed_child_native_outcome::CANCELLED && m_state->phase == Phase::GRANTED))
        {
            m_state->error = {Managed_child_native_error_domain::PROVIDER, 0, "Invalid elevated native completion"};
            return false;
        }
        const auto* occurrence = m_state->custody->find_occurrence_locked(
            m_state->identity.process_instance_id, m_state->identity.occurrence);
        if (outcome == Managed_child_native_outcome::EXITED && !occurrence->native.exited()) {
            m_state->error = observation_error.domain != Managed_child_native_error_domain::NONE
                ? std::move(observation_error)
                : Managed_child_native_error{Managed_child_native_error_domain::PROVIDER, 0,
                      "Native child exit not confirmed"};
            return false;
        }
        settled = detail::settle_native_elevation_locked(*m_state, outcome, std::move(error));
    }
    m_state->binding_changed.notify_all();
    m_state->custody->changed.notify_all();
    return settled;
}

inline Managed_child_native_elevation_request Managed_process::request_child_native_elevation(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    const Managed_child_occurrence_identity& identity,
    std::chrono::steady_clock::time_point deadline)
{
    Managed_child_native_elevation_request result;
#ifdef _WIN32
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now || deadline == std::chrono::steady_clock::time_point::max()) {
        result.request.error = {Managed_child_native_error_domain::PROVIDER, 0, "Elevation requires a finite future deadline"};
        return result;
    }
    auto state = std::make_shared<detail::Managed_child_native_elevation_state>();
    state->custody = custody;
    state->identity = identity;
    state->deadline = std::min(deadline, now + std::chrono::minutes(2));
    // Allocate the caller's ownership before publishing an active reservation.
    auto ticket = std::unique_ptr<Managed_child_native_elevation_ticket>(
        new Managed_child_native_elevation_ticket(state));
    uint64_t release_generation = 0;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        auto* occurrence = custody->find_occurrence_locked(identity.process_instance_id, identity.occurrence);
        if (identity.custody_identity != custody->identity || !occurrence) {
            result.request.error = {Managed_child_native_error_domain::PROVIDER, 0, "Elevation occurrence mismatch"};
            return result;
        }
        if (occurrence->native.exited()) {
            result.request.admission = Managed_child_native_admission::ALREADY_EXITED;
            result.request.action_generation = occurrence->native_action.generation;
            return result;
        }
        if (occurrence->native_action.outcome == Managed_child_native_outcome::ACTIVE) {
            result.request.admission = Managed_child_native_admission::ALREADY_ACTIVE;
            result.request.action_generation = occurrence->native_action.generation;
            return result;
        }
        if (occurrence->setup != detail::Managed_child_occurrence_record::setup_state::ownership_ready ||
            !occurrence->native.process_handle_owned())
        {
            result.request.error = {Managed_child_native_error_domain::PROVIDER, 0, "Elevation native ownership is not ready"};
            return result;
        }
        HANDLE reference = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(occurrence->native.process_handle()),
            GetCurrentProcess(), &reference, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
        {
            const DWORD error = GetLastError();
            result.request.error = {Managed_child_native_error_domain::WINDOWS, (int)error, "DuplicateHandle(elevation reference)"};
            return result;
        }
        state->reference_handle = reinterpret_cast<uintptr_t>(reference);
        state->target_pid = (uint32_t)occurrence->native.pid();
        release_generation = custody->release_state.request(detail::Release_mode::cleanup);
        custody->readiness_cancelled.store(true, std::memory_order_release);
        occurrence->native_recovery_requested = true;
        state->generation = detail::begin_child_native_action_locked(*occurrence);
        occurrence->native_action.stage = Managed_child_native_stage::ELEVATION_PENDING;
    }
    try {
        // Prepare observation before exposing a ticket that can launch a broker.
        // Binding needs no later worker allocation, even after delayed consent.
        start_owned_lifecycle_worker([this, state]() {
            {
                std::unique_lock<std::mutex> lock(state->custody->mutex);
                state->binding_changed.wait(lock, [&]() {
                    return state->broker_handle != 0 || state->abandoned ||
                        state->phase == detail::Managed_child_native_elevation_state::Phase::SETTLED;
                });
                if (state->broker_handle == 0) {
                    return;
                }
            }
            const DWORD wait = WaitForSingleObject(reinterpret_cast<HANDLE>(state->broker_handle), INFINITE);
            const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            Managed_child_native_error observation_error;
            if (wait == WAIT_OBJECT_0) {
                observation_error = detail::observe_elevated_child_exit(*this, *state);
            }
            {
                std::lock_guard<std::mutex> lock(state->custody->mutex);
                state->broker_exited = wait == WAIT_OBJECT_0;
                if (state->broker_exited && state->abandoned) {
                    detail::settle_native_elevation_locked(*state, Managed_child_native_outcome::FAILED,
                        observation_error.domain != Managed_child_native_error_domain::NONE
                            ? observation_error
                            : Managed_child_native_error{Managed_child_native_error_domain::PROVIDER, 0,
                                  "Elevation broker exited after controller loss"});
                }
                else
                if (!state->broker_exited) {
                    state->error = {Managed_child_native_error_domain::WINDOWS, (int)error,
                        "WaitForSingleObject(broker)"};
                }
            }
            state->custody->changed.notify_all();
        }, detail::test_hooks::k_managed_child_elevation_observer_start,
            identity.process_instance_id, identity.occurrence);
    }
    catch (const std::exception& exception) {
        result.request.error = {Managed_child_native_error_domain::PROVIDER, 0, exception.what()};
        {
            std::lock_guard<std::mutex> lock(custody->mutex);
            detail::settle_native_elevation_locked(*state, Managed_child_native_outcome::FAILED, result.request.error);
        }
        custody->changed.notify_all();
        if (release_generation != 0) {
            start_child_custody_release_worker(custody, release_generation);
        }
        return result;
    }
    custody->changed.notify_all();
    if (release_generation != 0) {
        start_child_custody_release_worker(custody, release_generation);
    }
    result.request.admission = Managed_child_native_admission::STARTED;
    result.request.action_generation = state->generation;
    result.ticket = std::move(ticket);
#else
    (void)custody;
    (void)identity;
    (void)deadline;
    result.request.error = {Managed_child_native_error_domain::PROVIDER, 0, "Native elevation is unavailable on this platform"};
#endif
    return result;
}

} // namespace sintra
