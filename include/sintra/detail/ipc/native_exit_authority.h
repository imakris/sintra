// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace sintra::detail {

/** Retains an armed exact native process reference and its captured lock word.
 * has_exited() is a non-blocking native observation or a retained native fact.
 * The reference must remain valid through destruction. Timeouts and PID-only
 * probes cannot implement this interface. Every true result imports that exact
 * actor's shared writes preceding exit before dependent shared-state reads.
 * A cached fact forwarded between observers needs release/acquire publication
 * after the native import. Retained pidfds do not reserve their numeric PIDs.
 * Infrastructure implementations only.
 */
class Native_exit_witness
{
public:
    explicit Native_exit_witness(uint64_t process_instance) noexcept

    :
        m_process_instance(process_instance)
    {}

    virtual ~Native_exit_witness() = default;
    uint64_t process_instance() const noexcept { return m_process_instance; }
    virtual bool has_exited() const noexcept = 0;

private:
    const uint64_t m_process_instance;
};

/** Immutable authority set, retained only by the binding of one ring occurrence.
 * Include any separately recoverable actor before that actor can take a lock.
 * A surviving owner retains this set and the notification record independently
 * of a disposable recoverer. No authority is cached by numeric PID.
 */
class Native_exit_authority
{
public:
    explicit Native_exit_authority(
        std::vector<std::shared_ptr<const Native_exit_witness>> witnesses)

    :
        m_witnesses(std::move(witnesses))
    {}

    bool contains(uint64_t process_instance) const noexcept
    {
        for (const auto& witness : m_witnesses) {
            if (witness && process_instance != 0 && witness->process_instance() == process_instance) {
                return true;
            }
        }
        return false;
    }

    bool has_exited(uint64_t process_instance) const noexcept
    {
        if (process_instance == 0) {
            return false;
        }
        for (const auto& witness : m_witnesses) {
            if (witness && witness->process_instance() == process_instance && witness->has_exited()) {
                return true;
            }
        }
        return false;
    }

private:
    const std::vector<std::shared_ptr<const Native_exit_witness>> m_witnesses;
};

} // namespace sintra::detail
