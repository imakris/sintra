#define private public
#include "sintra/detail/coordinator.h"
#include "sintra/detail/coordinator_impl.h"
#undef private

#include <iostream>
#include <mutex>
#include <unordered_set>

int main()
{
    using namespace sintra;

    Coordinator coordinator;
    s_coord = &coordinator;
    s_coord_id = coordinator.m_instance_id;

    const auto coordinator_process = make_process_instance_id(2);
    const auto initial_worker = make_process_instance_id(3);

    coordinator.make_process_group(
        "_sintra_all_processes",
        std::unordered_set<instance_id_type>{coordinator_process, initial_worker});
    coordinator.make_process_group(
        "_sintra_external_processes",
        std::unordered_set<instance_id_type>{initial_worker});

    const auto late_joiner = make_process_instance_id(4);

    {
        std::unique_lock<std::mutex> groups_lock(coordinator.m_groups_mutex);
        if (!coordinator.enroll_process_in_default_groups_locked(groups_lock, late_joiner, true)) {
            std::cerr << "join: enrollment in default groups failed" << std::endl;
            return 1;
        }

        if (!coordinator.m_groups["_sintra_all_processes"].m_process_ids.count(late_joiner)) {
            std::cerr << "ready: late joiner missing from _sintra_all_processes" << std::endl;
            return 1;
        }
        if (!coordinator.m_groups["_sintra_external_processes"].m_process_ids.count(late_joiner)) {
            std::cerr << "ready: late joiner missing from _sintra_external_processes" << std::endl;
            return 1;
        }

        if (!coordinator.enroll_process_in_default_groups_locked(groups_lock, late_joiner, true)) {
            std::cerr << "swing: re-enrollment should succeed" << std::endl;
            return 1;
        }

        coordinator.remove_process_from_group_locked(
            groups_lock, coordinator.m_groups["_sintra_external_processes"], late_joiner);
        coordinator.remove_process_from_group_locked(
            groups_lock, coordinator.m_groups["_sintra_all_processes"], late_joiner);

        if (coordinator.m_groups["_sintra_all_processes"].m_process_ids.count(late_joiner)) {
            std::cerr << "depart: late joiner remained in _sintra_all_processes" << std::endl;
            return 1;
        }
        if (coordinator.m_groups_of_process.find(late_joiner) != coordinator.m_groups_of_process.end()) {
            std::cerr << "cleanup: process-to-group mapping not cleared" << std::endl;
            return 1;
        }

        if (!coordinator.enroll_process_in_default_groups_locked(groups_lock, late_joiner, true)) {
            std::cerr << "cleanup: re-enrollment after removal failed" << std::endl;
            return 1;
        }
    }

    if (!coordinator.m_groups["_sintra_all_processes"].m_process_ids.count(late_joiner)) {
        std::cerr << "final: membership lost after cleanup" << std::endl;
        return 1;
    }
    if (!coordinator.m_groups["_sintra_external_processes"].m_process_ids.count(late_joiner)) {
        std::cerr << "final: external group membership missing after cleanup" << std::endl;
        return 1;
    }

    return 0;
}
