// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>


namespace sintra {

namespace detail {

// ---------------------------------------------------------------------------
// Shutdown protocol state (detail — not part of the public API)
// ---------------------------------------------------------------------------
// Tracks the lifecycle phase of the standard shutdown protocol within a single
// process.  The runtime uses this to reject illegal compositions such as
// calling detail::finalize() while a standard shutdown is in progress, or
// layering extra final barriers on top of an active shutdown protocol.
//
// Ordinary callers should not inspect or manipulate this state.  It is
// exposed in detail for runtime internals, barrier guardrails, and
// low-level tests that deliberately exercise protocol state transitions.
enum class shutdown_protocol_state : int {
    idle = 0,
    collective_shutdown_entered = 1,
    local_departure_entered     = 2,
    coordinator_hook_running    = 3,
    coordinator_hook_completed  = 4,
    finalizing                  = 5
};

} // namespace detail

using type_id_type     = uint64_t;
using instance_id_type = uint64_t;


struct Managed_process;
struct Coordinator;

///\brief Stores process-wide runtime pointers shared by façade helpers.
///
/// The Sintra runtime is built around a per-process singleton that keeps track
/// of the active managed process instance, the connected coordinator (if any),
/// and their associated identifiers.  Having a single well-defined location for
/// these pointers avoids global variables scattered across translation units
/// and makes the façade headers easier to reason about.
class runtime_state {
public:
    static runtime_state& instance() noexcept
    {
        static runtime_state state;
        return state;
    }

    Managed_process*& managed_process_ref()     noexcept { return m_managed_process;    }
    Coordinator*& coordinator_ref()             noexcept { return m_coordinator;        }
    instance_id_type& managed_process_id_ref()  noexcept { return m_managed_process_id; }
    instance_id_type& coordinator_id_ref()      noexcept { return m_coordinator_id;     }

    // Internal: runtime code accesses this through detail::s_shutdown_state.
    std::atomic<detail::shutdown_protocol_state>& shutdown_state_ref() noexcept { return m_shutdown_state; }
    std::mutex& teardown_admission_mutex_ref() noexcept { return m_teardown_admission_mutex; }

    Managed_process* managed_process()    const noexcept { return m_managed_process;    }
    Coordinator* coordinator()            const noexcept { return m_coordinator;        }
    instance_id_type managed_process_id() const noexcept { return m_managed_process_id; }
    instance_id_type coordinator_id()     const noexcept { return m_coordinator_id;     }

private:
    Managed_process*   m_managed_process      = nullptr;
    Coordinator*       m_coordinator          = nullptr;
    instance_id_type   m_managed_process_id   = 0;
    instance_id_type   m_coordinator_id       = 0;
    std::atomic<detail::shutdown_protocol_state> m_shutdown_state{detail::shutdown_protocol_state::idle};
    std::mutex m_teardown_admission_mutex;
};

inline auto& s_mproc    = runtime_state::instance().managed_process_ref();
inline auto& s_coord    = runtime_state::instance().coordinator_ref();
inline auto& s_mproc_id = runtime_state::instance().managed_process_id_ref();
inline auto& s_coord_id = runtime_state::instance().coordinator_id_ref();

namespace detail {
inline auto& s_shutdown_state = runtime_state::instance().shutdown_state_ref();
inline auto& s_teardown_admission_mutex = runtime_state::instance().teardown_admission_mutex_ref();
} // namespace detail

}

