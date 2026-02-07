#include <sintra/sintra.h>

#include "test_utils.h"

#include <filesystem>
#include <string>

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    if (!s_coord) {
        sintra::finalize();
        return 1;
    }

    const auto before_inflight = s_coord->m_inflight_joins.size();
    const auto before_joined = s_coord->m_joined_process_branch.size();
    const auto before_groups = s_coord->m_groups_of_process.size();

    const auto missing_dir = sintra::test::unique_scratch_directory("join_swarm_failure");
    const auto missing_binary = (missing_dir / "definitely_missing_sintra_binary").string();

    const auto result = sintra::join_swarm(1, missing_binary);

    const bool inflight_ok = (s_coord->m_inflight_joins.size() == before_inflight);
    const bool joined_ok = (s_coord->m_joined_process_branch.size() == before_joined);
    const bool groups_ok = (s_coord->m_groups_of_process.size() == before_groups);

    sintra::finalize();

    if (result != sintra::invalid_instance_id) {
        return 1;
    }

    return (inflight_ok && joined_ok && groups_ok) ? 0 : 1;
}
