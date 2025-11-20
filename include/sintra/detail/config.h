// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstddef>

// Ring reading policy
// ===================

// The waiting policy is 3 phase, adaptive:
// - Phase 1: Fast spin (50μs) for ultra-low latency
// - Phase 2: Precision sleep cycles (1ms) for moderate latency with low CPU
// - Phase 3: True blocking sleep (semaphore) for no CPU burn on stalls
// Memory barriers ensure cache coherency even with stale shared memory.

namespace sintra {

    // instance_id_type is a 2-part integer variable. The first part identifies the process and
    // the second the transceiver in the process. This variable configures the number of bits
    // allocated to the process index part and consequently the number of bits of transceiver
    // part as well.
    // NOTE: This value affects the size of Ring::Control exponentially.
    // see definition of instance_id_type
    constexpr int       num_process_index_bits              = 8;

    // The reason for setting such restriction is that the coordinator has to keep track of all
    // globally visible (public) transceivers. If a process is misbehaving and allocates public
    // transceivers carelessly, this could compromise the stability of the system
    constexpr int       max_public_transceivers_per_proc    = 65535;

    // Similarly, there is no true restriction to what this value could be. However, allowing a
    // process to send messages arbitrarily large could compromise stability.
    constexpr int       max_message_length                  = 4096;

    // Adaptive policy: Fast spin duration before transitioning to precision sleep.
    // This should be very short to catch immediate responses with minimal latency.
    constexpr double    fast_spin_duration                  = 0.00005; // secs (50μs)

    // Adaptive policy: Precision sleep cycle duration (seconds).
    // Uses high-resolution sleep (timeBeginPeriod on Windows, nanosleep on Linux).
    constexpr double    precision_sleep_cycle               = 0.001;   // secs (1ms)

    // Adaptive policy: Total time to spend in precision sleep phase before
    // transitioning to true blocking sleep (semaphore wait).
    constexpr double    precision_sleep_duration            = 1.0;     // secs (1000ms)

    // Whenever control data is read and written in an array by multiple threads, the layout used
    // should not cause cache invalidations (false sharing). This setting is architecture specific,
    // but it's not really that different among different x86 CPUs.
    constexpr size_t    assumed_cache_line_size             = 0x40;
}


