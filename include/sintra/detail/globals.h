// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstdint>


namespace sintra {

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

    Managed_process* managed_process()    const noexcept { return m_managed_process;    }
    Coordinator* coordinator()            const noexcept { return m_coordinator;        }
    instance_id_type managed_process_id() const noexcept { return m_managed_process_id; }
    instance_id_type coordinator_id()     const noexcept { return m_coordinator_id;     }

private:
    Managed_process*   m_managed_process      = nullptr;
    Coordinator*       m_coordinator          = nullptr;
    instance_id_type   m_managed_process_id   = 0;
    instance_id_type   m_coordinator_id       = 0;
};

#define s_mproc    sintra::runtime_state::instance().managed_process_ref()
#define s_coord    sintra::runtime_state::instance().coordinator_ref()
#define s_mproc_id sintra::runtime_state::instance().managed_process_id_ref()
#define s_coord_id sintra::runtime_state::instance().coordinator_id_ref()

}

