#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace std::chrono_literals;

using sintra::s_coord;

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    if (!s_coord) {
        sintra::detail::finalize();
        return 1;
    }

    const auto before_inflight = s_coord->m_inflight_joins.size();
    const auto before_joined   = s_coord->m_joined_process_branch.size();
    const auto before_groups   = s_coord->m_groups_of_process.size();
    const auto custody_count = []() {
        std::lock_guard lock(sintra::s_mproc->m_child_custody_mutex);
        return sintra::s_mproc->m_child_custodies.size();
    };
    const auto initialization_count = []() {
        std::lock_guard lock(s_coord->m_init_tracking_mutex);
        return s_coord->m_processes_in_initialization.size();
    };
    const auto before_custodies     = custody_count();
    const auto before_initializing  = initialization_count();

    const auto missing_dir     = sintra::test::unique_scratch_directory("join_swarm_failure");
    const auto missing_binary  = (missing_dir / "definitely_missing_sintra_binary").string();
    const auto result          = sintra::join_swarm(1, missing_binary);

    const bool inflight_ok     = (s_coord->m_inflight_joins.size() == before_inflight);
    const bool joined_ok       = (s_coord->m_joined_process_branch.size() == before_joined);
    const bool groups_ok       = (s_coord->m_groups_of_process.size() == before_groups);
    const auto custody_deadline = std::chrono::steady_clock::now() + 5s;
    while (
        custody_count() != before_custodies                      &&
        std::chrono::steady_clock::now() < custody_deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    const bool custody_ok       = custody_count() == before_custodies;
    const bool initialization_ok = initialization_count() == before_initializing;

    sintra::detail::finalize();

    if (result != sintra::invalid_instance_id) {
        return 1;
    }

    return (inflight_ok && joined_ok && groups_ok && custody_ok && initialization_ok)
        ? 0
        : 1;
}
