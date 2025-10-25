#include "sintra/detail/coordinator_impl.h"

#include <initializer_list>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

using sintra::detail::Coordinator_group_membership;
using sintra::instance_id_type;

struct Dummy_group
{
    instance_id_type m_instance_id = 0;
    std::unordered_set<instance_id_type> m_process_ids;

    instance_id_type instance_id() const noexcept
    {
        return m_instance_id;
    }

    void add_process(instance_id_type process_iid)
    {
        m_process_ids.insert(process_iid);
    }

    void remove_process(instance_id_type process_iid)
    {
        m_process_ids.erase(process_iid);
    }
};

class Membership_fixture
{
public:
    Membership_fixture()
    {
        const auto coordinator_process = sintra::make_process_instance_id(2);
        const auto initial_worker = sintra::make_process_instance_id(3);

        add_group("_sintra_all_processes", {coordinator_process, initial_worker});
        add_group("_sintra_external_processes", {initial_worker});
    }

    bool enroll(instance_id_type process_iid, bool include_external)
    {
        return Coordinator_group_membership::enroll_in_default_groups(
            groups_of_process,
            groups,
            process_iid,
            include_external);
    }

    void remove_from_group(const std::string& name, instance_id_type process_iid)
    {
        auto it = groups.find(name);
        if (it != groups.end()) {
            Coordinator_group_membership::remove_process(groups_of_process, it->second, process_iid);
        }
    }

    Dummy_group& group(const std::string& name)
    {
        return groups[name];
    }

    const Dummy_group& group(const std::string& name) const
    {
        return groups.at(name);
    }

    const auto& membership_map() const noexcept { return groups_of_process; }

private:
    void add_group(const std::string& name, std::initializer_list<instance_id_type> members)
    {
        Dummy_group group;
        group.m_instance_id = next_group_instance_id++;

        for (auto process : members) {
            group.add_process(process);
            groups_of_process[process].insert(group.m_instance_id);
        }

        groups.emplace(name, std::move(group));
    }

    instance_id_type next_group_instance_id = 1000;
    std::unordered_map<std::string, Dummy_group> groups;
    std::unordered_map<instance_id_type, std::unordered_set<instance_id_type>> groups_of_process;
};

} // namespace

int main()
{
    Membership_fixture fixture;
    const auto late_joiner = sintra::make_process_instance_id(4);

    if (!fixture.enroll(late_joiner, true)) {
        std::cerr << "join: enrollment in default groups failed" << std::endl;
        return 1;
    }

    if (!fixture.group("_sintra_all_processes").m_process_ids.count(late_joiner)) {
        std::cerr << "ready: late joiner missing from _sintra_all_processes" << std::endl;
        return 1;
    }
    if (!fixture.group("_sintra_external_processes").m_process_ids.count(late_joiner)) {
        std::cerr << "ready: late joiner missing from _sintra_external_processes" << std::endl;
        return 1;
    }

    if (!fixture.enroll(late_joiner, true)) {
        std::cerr << "swing: re-enrollment should succeed" << std::endl;
        return 1;
    }

    fixture.remove_from_group("_sintra_external_processes", late_joiner);
    fixture.remove_from_group("_sintra_all_processes", late_joiner);

    if (fixture.group("_sintra_all_processes").m_process_ids.count(late_joiner)) {
        std::cerr << "depart: late joiner remained in _sintra_all_processes" << std::endl;
        return 1;
    }
    if (fixture.membership_map().find(late_joiner) != fixture.membership_map().end()) {
        std::cerr << "cleanup: process-to-group mapping not cleared" << std::endl;
        return 1;
    }

    if (!fixture.enroll(late_joiner, true)) {
        std::cerr << "cleanup: re-enrollment after removal failed" << std::endl;
        return 1;
    }

    if (!fixture.group("_sintra_all_processes").m_process_ids.count(late_joiner)) {
        std::cerr << "final: membership lost after cleanup" << std::endl;
        return 1;
    }
    if (!fixture.group("_sintra_external_processes").m_process_ids.count(late_joiner)) {
        std::cerr << "final: external group membership missing after cleanup" << std::endl;
        return 1;
    }

    return 0;
}
