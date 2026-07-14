// Fail-first contract for exact managed-child publication retirement.
//
// A service request from occurrence N must not retire the publication that
// currently belongs to occurrence N+1, even when both use the same raw IID.

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

constexpr std::uint64_t k_custody_identity = 0x5a71u;
constexpr std::uint32_t k_stale_occurrence = 7;
constexpr std::uint32_t k_replacement_occurrence = 8;
constexpr std::uint64_t k_transceiver_index = 0x6a51u;

std::atomic<unsigned> s_retirement_count{0};
sintra::instance_id_type s_expected_iid = sintra::invalid_instance_id;
std::string s_expected_name;

void observe_name_retirement(
    sintra::instance_id_type instance_id,
    const std::string&       assigned_name)
{
    if (instance_id == s_expected_iid && assigned_name == s_expected_name) {
        s_retirement_count.fetch_add(1, std::memory_order_release);
    }
}

class Scoped_current_message
{
public:
    explicit Scoped_current_message(sintra::Message_prefix* message) noexcept
        : m_previous(sintra::s_tl_current_message)
    {
        sintra::s_tl_current_message = message;
    }

    ~Scoped_current_message() noexcept
    {
        sintra::s_tl_current_message = m_previous;
    }

    Scoped_current_message(const Scoped_current_message&) = delete;
    Scoped_current_message& operator=(const Scoped_current_message&) = delete;

private:
    sintra::Message_prefix* m_previous;
};

sintra::instance_id_type resolve_exact(
    const std::string&       assigned_name,
    sintra::instance_id_type process_iid,
    std::uint32_t            occurrence)
{
    std::atomic<bool> cancelled{false};
    return sintra::detail::Managed_child_readiness_access::resolve(
        sintra::s_coord,
        assigned_name,
        k_custody_identity,
        process_iid,
        occurrence,
        cancelled);
}

struct Publication_snapshot
{
    bool registry_present = false;
};

Publication_snapshot publication_snapshot(
    sintra::instance_id_type process_iid,
    sintra::instance_id_type instance_iid)
{
    Publication_snapshot snapshot;
    std::lock_guard lock(sintra::s_coord->m_publish_mutex);

    const auto process = sintra::s_coord->m_transceiver_registry.find(process_iid);
    snapshot.registry_present =
        process != sintra::s_coord->m_transceiver_registry.end() &&
        process->second.count(instance_iid) != 0;

    return snapshot;
}

sintra::instance_id_type resolve_ordinary(const std::string& assigned_name)
{
    return sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id, assigned_name);
}

bool no_managed_child_authority()
{
    if (!sintra::s_mproc) {
        return false;
    }

    bool custody_empty = false;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
        custody_empty = sintra::s_mproc->m_child_custodies.empty() &&
            sintra::s_mproc->m_child_custody_by_process.empty();
    }

    bool cached_spawns_empty = false;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_cached_spawns_mutex);
        cached_spawns_empty = sintra::s_mproc->m_cached_spawns.empty();
    }

    bool lifelines_empty = false;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_lifeline_mutex);
        lifelines_empty = sintra::s_mproc->m_lifeline_writes.empty();
    }

#ifndef _WIN32
    bool reap_roster_empty = false;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_spawned_child_pids_mutex);
        reap_roster_empty = sintra::s_mproc->m_spawned_child_pids.empty();
    }
#else
    const bool reap_roster_empty = true;
#endif

    return custody_empty && cached_spawns_empty && lifelines_empty &&
        reap_roster_empty;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    const auto process_iid = sintra::s_mproc_id;
    const auto replacement_iid = sintra::compose_instance(
        sintra::get_process_index(process_iid),
        k_transceiver_index);
    const std::string replacement_name =
        "managed_child_stale_unpublish_replacement_" +
        std::to_string(static_cast<unsigned long long>(process_iid));

    s_expected_iid = replacement_iid;
    s_expected_name = replacement_name;
    s_retirement_count.store(0, std::memory_order_relaxed);

    const auto publish_result =
        sintra::s_coord->publish_managed_child_transceiver_for_test(
            sintra::make_user_type_id(1003),
            replacement_iid,
            replacement_name,
            k_custody_identity,
            process_iid,
            k_replacement_occurrence);
    const auto before = publication_snapshot(process_iid, replacement_iid);
    const bool baseline_valid = publish_result == replacement_iid &&
        resolve_ordinary(replacement_name) == replacement_iid &&
        resolve_exact(
            replacement_name, process_iid, k_replacement_occurrence) ==
                replacement_iid &&
        resolve_exact(replacement_name, process_iid, k_stale_occurrence) ==
            sintra::invalid_instance_id &&
        before.registry_present && no_managed_child_authority();

    bool stale_result = false;
    sintra::Message_prefix* const previous_message = sintra::s_tl_current_message;
    {
        sintra::test::managed_child::Scoped_test_hook retirement_hook(
            sintra::detail::test_hooks::s_coordinator_name_retired,
            &observe_name_retirement);

        sintra::Message_prefix stale_request{};
        stale_request.message_type_id = static_cast<sintra::type_id_type>(
            sintra::detail::reserved_id::unpublish_transceiver);
        stale_request.sender_instance_id = process_iid;
        stale_request.receiver_instance_id = sintra::s_coord_id;
        stale_request.managed_child_custody_identity = k_custody_identity;
        stale_request.managed_child_occurrence = k_stale_occurrence;

        Scoped_current_message current_message(&stale_request);
        stale_result = sintra::Coordinator::rpc_unpublish_transceiver(
            sintra::s_coord_id, replacement_iid);
    }

    const bool tls_restored = sintra::s_tl_current_message == previous_message;
    const bool hook_restored =
        sintra::detail::test_hooks::s_coordinator_name_retired.load(
            std::memory_order_acquire) == nullptr;
    const unsigned retirement_count =
        s_retirement_count.load(std::memory_order_acquire);
    const auto after_stale = publication_snapshot(process_iid, replacement_iid);
    const auto ordinary_after_stale = resolve_ordinary(replacement_name);
    const auto replacement_exact_after_stale = resolve_exact(
        replacement_name, process_iid, k_replacement_occurrence);
    const auto stale_exact_after_stale = resolve_exact(
        replacement_name, process_iid, k_stale_occurrence);

    const bool fixed_green = baseline_valid && !stale_result &&
        retirement_count == 0 && ordinary_after_stale == replacement_iid &&
        replacement_exact_after_stale == replacement_iid &&
        stale_exact_after_stale == sintra::invalid_instance_id &&
        after_stale.registry_present && tls_restored && hook_restored;

    const bool current_red = baseline_valid && stale_result &&
        retirement_count == 1 &&
        ordinary_after_stale == sintra::invalid_instance_id &&
        replacement_exact_after_stale == sintra::invalid_instance_id &&
        stale_exact_after_stale == sintra::invalid_instance_id &&
        after_stale.registry_present && tls_restored && hook_restored;

    // GREEN still owns the replacement publication; RED has already damaged
    // it. Successful cleanup must retire the complete owning registry record.
    const bool cleanup_result = sintra::Coordinator::rpc_unpublish_transceiver(
        sintra::s_coord_id, replacement_iid);
    const auto after_cleanup = publication_snapshot(process_iid, replacement_iid);
    const bool cleanup_valid = cleanup_result && !after_cleanup.registry_present &&
        resolve_ordinary(replacement_name) == sintra::invalid_instance_id &&
        resolve_exact(
            replacement_name, process_iid, k_replacement_occurrence) ==
                sintra::invalid_instance_id &&
        !after_cleanup.registry_present && no_managed_child_authority();

    const bool finalized = sintra::detail::finalize();
    const bool runtime_gone = sintra::s_mproc == nullptr && sintra::s_coord == nullptr;

    if (current_red && cleanup_valid && finalized && runtime_gone) {
        std::fprintf(stderr,
            "STALE_UNPUBLISH_RED custody=%llu piid=%llu iid=%llu "
            "occurrence_n=%u occurrence_n1=%u stale_result=1 retired=1 "
            "ordinary_n1=0 exact_n1=0 registry_cell_n1=1 identity_n1=0 "
            "cleanup=1 finalized=1 native_children=0 hooks_restored=1 "
            "tls_restored=1\n",
            static_cast<unsigned long long>(k_custody_identity),
            static_cast<unsigned long long>(process_iid),
            static_cast<unsigned long long>(replacement_iid),
            k_stale_occurrence,
            k_replacement_occurrence);
        std::fflush(stderr);
        return 87;
    }

    if (fixed_green && cleanup_valid && finalized && runtime_gone) {
        std::fprintf(stdout,
            "STALE_UNPUBLISH_GREEN custody=%llu piid=%llu iid=%llu "
            "occurrence_n=%u occurrence_n1=%u stale_result=0 retired=0 "
            "ordinary_n1=1 exact_n1=1 registry_cell_n1=1 identity_n1=1 "
            "cleanup=1 finalized=1 native_children=0 hooks_restored=1 "
            "tls_restored=1\n",
            static_cast<unsigned long long>(k_custody_identity),
            static_cast<unsigned long long>(process_iid),
            static_cast<unsigned long long>(replacement_iid),
            k_stale_occurrence,
            k_replacement_occurrence);
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(stderr,
        "STALE_UNPUBLISH_INVALID baseline=%d stale_result=%d retired=%u "
        "ordinary_n1=%d exact_n1=%d stale_exact=%d registry_n1=%d "
        "identity_n1=%d identity_exact=%d tls_restored=%d hook_restored=%d "
        "fixed_green=%d current_red=%d cleanup_result=%d registry_absent=%d "
        "cleanup_valid=%d "
        "finalized=%d runtime_gone=%d\n",
        baseline_valid ? 1 : 0,
        stale_result ? 1 : 0,
        retirement_count,
        ordinary_after_stale == replacement_iid ? 1 : 0,
        replacement_exact_after_stale == replacement_iid ? 1 : 0,
        stale_exact_after_stale == sintra::invalid_instance_id ? 1 : 0,
        after_stale.registry_present ? 1 : 0,
        replacement_exact_after_stale != sintra::invalid_instance_id ? 1 : 0,
        replacement_exact_after_stale == replacement_iid ? 1 : 0,
        tls_restored ? 1 : 0,
        hook_restored ? 1 : 0,
        fixed_green ? 1 : 0,
        current_red ? 1 : 0,
        cleanup_result ? 1 : 0,
        !after_cleanup.registry_present ? 1 : 0,
        cleanup_valid ? 1 : 0,
        finalized ? 1 : 0,
        runtime_gone ? 1 : 0);
    std::fflush(stderr);
    return 3;
}
