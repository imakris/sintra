// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "ipc/rings.h"

#include <cstdint>
#include <functional>
#include <utility>

namespace sintra {

using boot_id_type = uint64_t;

enum class barrier_state : uint8_t {
    not_requested = 0,
    satisfied = 1,
    downgraded = 2,
    failed = 3,
};

enum class barrier_failure : uint8_t {
    none = 0,
    timeout = 1,
    peer_draining = 2,
    peer_lost = 3,
    coordinator_stop = 4,
    incompatible_request = 5,
};

struct barrier_phase_status {
    barrier_state         state;
    barrier_failure       failure_code;
    sequence_counter_type sequence;
};

struct barrier_completion_payload {
    uint64_t              barrier_epoch;
    sequence_counter_type barrier_sequence;
    uint32_t              requirement_mask;

    barrier_phase_status  rendezvous;
    barrier_phase_status  outbound;
    barrier_phase_status  processing;
};

constexpr uint32_t barrier_flag_none = 0;
constexpr uint32_t barrier_flag_inbound = 1u << 0;
constexpr uint32_t barrier_flag_outbound = 1u << 1;
constexpr uint32_t barrier_flag_processing = 1u << 2;

constexpr barrier_phase_status make_phase_status(
    barrier_state state = barrier_state::not_requested,
    barrier_failure failure = barrier_failure::none,
    sequence_counter_type sequence = invalid_sequence) noexcept
{
    return barrier_phase_status{state, failure, sequence};
}

constexpr barrier_completion_payload make_barrier_completion_payload() noexcept
{
    return barrier_completion_payload{
        0,
        invalid_sequence,
        0,
        make_phase_status(),
        make_phase_status(),
        make_phase_status(),
    };
}

struct barrier_result {
    barrier_completion_payload payload = make_barrier_completion_payload();
    bool                       local_delivery_satisfied = true;
    bool                       local_processing_satisfied = true;

    barrier_result() = default;

    explicit barrier_result(barrier_completion_payload payload_) noexcept
        : payload(std::move(payload_))
    {
    }

    sequence_counter_type sequence() const noexcept
    {
        return payload.barrier_sequence;
    }

    bool succeeded() const noexcept
    {
        const bool rendezvous_ok = payload.rendezvous.state == barrier_state::satisfied;
        const bool inbound_requested = (payload.requirement_mask & barrier_flag_inbound) != 0U;
        const bool outbound_ok = !inbound_requested ||
                                 payload.outbound.state == barrier_state::satisfied;
        const bool processing_requested = (payload.requirement_mask & barrier_flag_processing) != 0U;
        const bool processing_ok = !processing_requested ||
                                   payload.processing.state == barrier_state::satisfied;

        return rendezvous_ok &&
               outbound_ok &&
               processing_ok &&
               local_delivery_satisfied &&
               local_processing_satisfied;
    }

    explicit operator bool() const noexcept
    {
        return succeeded();
    }

    const barrier_completion_payload& completion() const noexcept
    {
        return payload;
    }
};

struct Participant_id {
    instance_id_type iid{};
    boot_id_type boot_id{};

    bool operator==(const Participant_id& o) const noexcept
    {
        return iid == o.iid && boot_id == o.boot_id;
    }
};

struct Participant_id_hash {
    size_t operator()(const Participant_id& p) const noexcept
    {
        size_t h1 = std::hash<instance_id_type>{}(p.iid);
        size_t h2 = std::hash<boot_id_type>{}(p.boot_id);
        return h1 * 1315423911u ^ h2;
    }
};

} // namespace sintra

