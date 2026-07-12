#pragma once

#include <sintra/detail/ipc/process_utils.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace sintra::test::managed_child {

struct Occurrence_isolation_identity
{
    std::string             nonce;
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    sintra::instance_id_type witness_iid = sintra::invalid_instance_id;
    std::uint32_t            occurrence = 0;
    int                      pid = -1;
    bool                     start_stamp_available = false;
    std::uint64_t            start_stamp = 0;
    std::string              witness_name;
};

struct Occurrence_isolation_outcome
{
    bool ledger_0_valid = false;
    bool predecessor_live = false;
    bool predecessor_request_held = false;
    bool recovery_observed = false;
    bool exact_crash = false;
    bool predecessor_name_retired = false;
    bool predecessor_request_still_held = false;
    bool ledger_1_valid = false;
    bool replacement_live = false;
    bool replacement_satisfied_predecessor = false;
    bool request_completed = false;
    bool request_threw = false;
    std::uint32_t request_result = 0;

    bool custody_record_found = false;
    bool occurrence_count_exact = false;
    bool predecessor_occurrence = false;
    bool predecessor_setup = false;
    bool predecessor_pid = false;
    bool predecessor_stamp_available = false;
    bool predecessor_stamp = false;
    bool predecessor_publication_retired = false;
    bool predecessor_communication_retired = false;
    bool predecessor_exit_confirmed = false;
    bool replacement_occurrence = false;
    bool replacement_setup = false;
    bool replacement_pid = false;
    bool replacement_stamp_available = false;
    bool replacement_stamp = false;
    bool replacement_created = false;
    bool replacement_not_exited = false;

    bool release_marker_written = false;
    bool replacement_finalize_marker = false;
    bool root_finalized = false;
    bool predecessor_abnormal_exit = false;
    bool replacement_normal_exit = false;
    bool forced_cleanup = false;
    bool survivors_absent = false;
    bool custody_release_complete = false;

    Occurrence_isolation_identity predecessor;
    Occurrence_isolation_identity replacement;
};

using Occurrence_isolation_outcome_callback = void (*)(
    const Occurrence_isolation_outcome&);

inline std::atomic<Occurrence_isolation_outcome_callback>
    s_occurrence_isolation_outcome{nullptr};

inline void emit_occurrence_isolation_outcome(
    const Occurrence_isolation_outcome& outcome)
{
    if (const auto callback = s_occurrence_isolation_outcome.load(
            std::memory_order_acquire))
    {
        callback(outcome);
    }
}

template <typename Callback>
class Scoped_test_hook
{
public:
    Scoped_test_hook(
        std::atomic<Callback>& slot,
        Callback               callback) noexcept
        : m_slot(&slot),
          m_previous(slot.exchange(callback, std::memory_order_acq_rel))
    {}

    ~Scoped_test_hook() noexcept
    {
        restore();
    }

    Scoped_test_hook(const Scoped_test_hook&) = delete;
    Scoped_test_hook& operator=(const Scoped_test_hook&) = delete;
    Scoped_test_hook(Scoped_test_hook&&) = delete;
    Scoped_test_hook& operator=(Scoped_test_hook&&) = delete;

    void restore() noexcept
    {
        if (m_slot) {
            m_slot->store(m_previous, std::memory_order_release);
            m_slot = nullptr;
        }
    }

private:
    std::atomic<Callback>* m_slot;
    Callback               m_previous;
};

inline bool exact_process_is_live(int pid, std::uint64_t start_stamp)
{
    if (pid <= 0 || !sintra::is_process_alive(static_cast<std::uint32_t>(pid))) {
        return false;
    }
    const auto observed =
        sintra::query_process_start_stamp(static_cast<std::uint32_t>(pid));
    return observed && *observed == start_stamp;
}

inline bool write_complete_file(
    const std::filesystem::path& path,
    const std::string&           contents)
{
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << contents;
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace sintra::test::managed_child
