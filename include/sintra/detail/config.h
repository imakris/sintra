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

#ifndef SINTRA_CONFIG_H
#define SINTRA_CONFIG_H

#include <cstddef>

// Ring reading policy
// ===================

// The waiting policy is 3 phase, adaptive:
// - Phase 1: Fast spin (50μs) for ultra-low latency
// - Phase 2: Precision sleep cycles (1ms) for moderate latency with low CPU
// - Phase 3: True blocking sleep (semaphore) for no CPU burn on stalls
// Memory barriers ensure cache coherency even with stale shared memory.

#ifndef __clang__ 
#define SINTRA_USE_OMP_GET_WTIME
#endif

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


#endif
