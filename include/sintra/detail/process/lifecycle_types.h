// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.
#pragma once

#include "../id_types.h"

#include <functional>

namespace sintra {

namespace detail {

enum class Member_lifetime_role
{
    COORDINATOR_BOUND,
    DETACHED
};

enum class Member_lifecycle_notice_kind : uint64_t
{
    MANAGED_DETACH_PRECOMMIT = 1,
    MANAGED_DETACH_ABORT = 2,
    COLLECTIVE_DEPARTURE = 3
};

inline constexpr unsigned char k_lifeline_release_byte = 0x01;

enum class Lifeline_observation
{
    INACTIVE,
    ARMED,
    DETACHED,
    BREACHED
};

struct External_process_claim_result
{
    uint64_t accepted;
    uint64_t role;
    uint64_t coordinator_pid;
    uint64_t coordinator_start_stamp;
    uint64_t exact_watch_required;
};

} // namespace detail

struct member_lifecycle_event
{
    enum class kind
    {
        LIFELINE_RELEASED,
        COORDINATOR_DEPARTED
    };

    enum class departure_cause
    {
        NONE,
        UNPUBLISHED,
        SIGNALED_CRASH,
        EXACT_OS_WATCH,
        COLLECTIVE_SHUTDOWN
    };

    kind            why = kind::LIFELINE_RELEASED;
    departure_cause cause = departure_cause::NONE;
};

using Member_lifecycle_handler =
    std::function<void(const member_lifecycle_event&)>;

enum class Managed_child_custody_state
{
    not_started,
    owner_bound,
    detaching,
    disowned
};

enum class Managed_child_detach_result
{
    not_started,
    settlement_pending,
    disowned,
    definite_non_delivery,
    no_live_occurrence,
    conflict
};

// Lifecycle/recovery metadata and callbacks (effective only in the coordinator).
// See docs/process_lifecycle_notes.md for timing and threading.
struct Crash_info
{
    // Process instance id and stable process slot for correlation.
    instance_id_type       process_iid  = invalid_instance_id;
    uint32_t               process_slot = 0;
    // Crash status or signal code (0 for non-crash paths).
    int                    status       = 0;
};

struct process_lifecycle_event
{
    // Emitted by the coordinator when a process crashes, exits normally, or
    // is unpublished without a prior draining/crash signal.
    enum class reason
    {
        crash,
        normal_exit,
        unpublished
    };

    instance_id_type       process_iid  = invalid_instance_id;
    uint32_t               process_slot = 0;
    reason                 why          = reason::unpublished;
    int                    status       = 0;
};

struct Recovery_control
{
    // True when shutdown has begun; runners should exit early.
    std::function<bool()>  should_cancel;
    // Spawn the replacement process once (subsequent calls are ignored).
    std::function<void()>  spawn;
};

// Called on the coordinator thread to decide whether recovery should occur.
using Recovery_policy = std::function<bool(const Crash_info&)>;
// Called on a recovery thread to perform any delay, countdown, or custom logic.
using Recovery_runner = std::function<void(const Crash_info&, const Recovery_control&)>;
// Called on the coordinator thread for crash/normal exit/unpublished events.
using Lifecycle_handler = std::function<void(const process_lifecycle_event&)>;

} // namespace sintra
