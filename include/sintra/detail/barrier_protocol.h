// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "id_types.h"

#include <cstdint>

namespace sintra::detail {

enum class barrier_state : std::uint8_t {
    not_requested = 0,
    satisfied = 1,
    downgraded = 2,
    failed = 3,
};

enum class barrier_failure : std::uint8_t {
    none = 0,
    timeout = 1,
    peer_draining = 2,
    peer_lost = 3,
    coordinator_stop = 4,
};

struct barrier_phase_status {
    barrier_state state = barrier_state::not_requested;
    barrier_failure failure_code = barrier_failure::none;
    instance_id_type offender = invalid_instance_id;
    sequence_counter_type sequence = invalid_sequence;
};

constexpr std::uint32_t barrier_flag_inbound = 1u << 0;
constexpr std::uint32_t barrier_flag_outbound = 1u << 1;
constexpr std::uint32_t barrier_flag_processing = 1u << 2;

struct barrier_completion_payload {
    sequence_counter_type barrier_sequence = invalid_sequence;
    std::uint32_t request_flags = 0;
    instance_id_type common_function_iid = invalid_instance_id;
    barrier_phase_status rendezvous {};
    barrier_phase_status inbound {};
    barrier_phase_status outbound {};
    barrier_phase_status processing {};
};

enum class barrier_ack_type : std::uint8_t {
    outbound = 1,
    processing = 2,
};

struct barrier_ack_request {
    sequence_counter_type barrier_sequence = invalid_sequence;
    instance_id_type common_function_iid = invalid_instance_id;
    barrier_ack_type ack_type = barrier_ack_type::outbound;
    sequence_counter_type target_sequence = invalid_sequence;
};

struct barrier_ack_response {
    sequence_counter_type barrier_sequence = invalid_sequence;
    instance_id_type common_function_iid = invalid_instance_id;
    barrier_ack_type ack_type = barrier_ack_type::outbound;
    sequence_counter_type observed_sequence = invalid_sequence;
    instance_id_type responder = invalid_instance_id;
    bool success = true;
};

} // namespace sintra::detail
