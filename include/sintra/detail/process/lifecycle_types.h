// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.
#pragma once

#include "../id_types.h"

#include <chrono>
#include <functional>

namespace sintra {

struct Crash_info
{
    instance_id_type process_iid = invalid_instance_id;
    uint32_t         process_slot = 0;
    int status = 0;
};

struct process_lifecycle_event
{
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

struct Recovery_action
{
    enum class kind
    {
        skip,
        immediate,
        delay
    };

    kind action = kind::immediate;
    std::chrono::milliseconds delay{0};

    static Recovery_action skip()
    {
        return {kind::skip, std::chrono::milliseconds(0)};
    }

    static Recovery_action immediate()
    {
        return {kind::immediate, std::chrono::milliseconds(0)};
    }

    static Recovery_action delay_for(std::chrono::milliseconds value)
    {
        return {kind::delay, value};
    }
};

using Recovery_policy = std::function<Recovery_action(const Crash_info&)>;      
using Recovery_tick_handler = std::function<void(const Crash_info&, int seconds_remaining)>;
using Recovery_cancel_handler = std::function<bool(const Crash_info&)>;
using Lifecycle_handler = std::function<void(const process_lifecycle_event&)>;  

} // namespace sintra
