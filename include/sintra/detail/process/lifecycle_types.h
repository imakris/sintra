// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.
#pragma once

#include "../id_types.h"

#include <functional>

namespace sintra {

// Lifecycle/recovery metadata and callbacks (effective only in the coordinator).
// See docs/process_lifecycle_notes.md for timing and threading.
struct Crash_info
{
    // Process instance id and stable process slot for correlation.
    instance_id_type process_iid = invalid_instance_id;
    uint32_t         process_slot = 0;
    // Crash status or signal code (0 for non-crash paths).
    int status = 0;
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

    instance_id_type process_iid = invalid_instance_id;
    uint32_t         process_slot = 0;
    reason           why = reason::unpublished;
    int              status = 0;
};

struct Recovery_control
{
    // True when shutdown has begun; runners should exit early.
    std::function<bool()> should_cancel;
    // Spawn the replacement process once (subsequent calls are ignored).
    std::function<void()> spawn;
};

// Called on the coordinator thread to decide whether recovery should occur.
using Recovery_policy = std::function<bool(const Crash_info&)>;
// Called on a recovery thread to perform any delay, countdown, or custom logic.
using Recovery_runner = std::function<void(const Crash_info&, const Recovery_control&)>;
// Called on the coordinator thread for crash/normal exit/unpublished events.
using Lifecycle_handler = std::function<void(const process_lifecycle_event&)>;

} // namespace sintra
