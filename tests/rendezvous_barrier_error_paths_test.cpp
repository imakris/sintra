#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;

std::string unique_group_name(const char* prefix)
{
    return std::string(prefix) + "_" + std::to_string(static_cast<unsigned long long>(s_mproc_id));
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    const auto previous_state = s_mproc->m_communication_state;
    s_mproc->m_communication_state = sintra::Managed_process::COMMUNICATION_PAUSED;

    bool ok = true;

    // Case 1: rpc_cancelled path in rendezvous_barrier.
    // Keep the group alive until finalize() so any late request reader dispatch
    // still finds a valid local receiver after cancellation.
    sintra::Process_group cancel_group;
    {
        const auto group_name = unique_group_name("rendezvous_cancel_group");
        const auto barrier_name = std::string("rendezvous_cancel_barrier");

        std::unordered_set<sintra::instance_id_type> members;
        members.insert(s_mproc_id);
        members.insert(sintra::make_process_instance_id(2));
        cancel_group.set(members);

        if (!cancel_group.assign_name(group_name)) {
            std::fprintf(stderr, "Failed to assign group name for cancel test.\n");
            ok = false;
        } else {
            std::atomic<bool> barrier_result{false};
            std::atomic<bool> barrier_done{false};
            std::thread waiter([&] {
                barrier_result = sintra::barrier<sintra::rendezvous_t>(barrier_name, group_name);
                barrier_done = true;
            });

            bool cancelled = false;
            const auto cancel_deadline = std::chrono::steady_clock::now() + 5s;
            while (!barrier_done.load() &&
                   std::chrono::steady_clock::now() < cancel_deadline)
            {
                if (s_mproc->unblock_rpc(sintra::process_of(s_coord_id)) > 0) {
                    cancelled = true;
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }

            waiter.join();

            if (!cancelled) {
                std::fprintf(stderr,
                             "Did not observe cancellable outstanding RPC (cancel test).\n");
                ok = false;
            }

            if (cancelled && !barrier_result.load()) {
                std::fprintf(stderr, "Expected rendezvous barrier to return true after cancellation.\n");
                ok = false;
            }
        }
    }

    // Case 2: runtime_error path in rendezvous_barrier (target shutting down).
    {
        sintra::Process_group group;
        const auto group_name = unique_group_name("rendezvous_shutdown_group");
        const auto barrier_name = std::string("rendezvous_shutdown_barrier");

        std::unordered_set<sintra::instance_id_type> members;
        members.insert(s_mproc_id);
        group.set(members);

        if (!group.assign_name(group_name)) {
            std::fprintf(stderr, "Failed to assign group name for shutdown test.\n");
            ok = false;
        } else {
            const auto group_iid = group.instance_id();
            group.destroy();
            s_mproc->m_instance_id_of_assigned_name[group_name] = group_iid;
            s_mproc->m_local_pointer_of_instance_id[group_iid] = &group;
            sintra::Transceiver::get_instance_to_object_map<sintra::Process_group::barrier_mftc>()[group_iid] = &group;

            const bool barrier_result =
                sintra::barrier<sintra::rendezvous_t>(barrier_name, group_name);

            if (!barrier_result) {
                std::fprintf(stderr, "Expected rendezvous barrier to return true for shutdown target.\n");
                ok = false;
            }

            {
                auto scoped = s_mproc->m_instance_id_of_assigned_name.scoped();
                scoped.get().erase(group_name);
            }
            {
                auto scoped = s_mproc->m_local_pointer_of_instance_id.scoped();
                scoped.get().erase(group_iid);
            }
            {
                auto scoped =
                    sintra::Transceiver::get_instance_to_object_map<sintra::Process_group::barrier_mftc>().scoped();
                scoped.get().erase(group_iid);
            }
        }
    }

    s_mproc->m_communication_state = previous_state;
    sintra::finalize();
    return ok ? 0 : 1;
}
