#include <sintra/sintra.h>

using namespace sintra;

namespace {

int controller_process()
{
    console() << "[Controller] requesting bench player\n";
    const auto bench_iid = spawn_branch(2);

    if (bench_iid == invalid_instance_id) {
        console() << "[Controller] failed to spawn bench player\n";
        deactivate_all_slots();
        return 0;
    }

    try {
        Coordinator::rpc_begin_process_draining(s_coord_id, bench_iid);
    }
    catch (...) {
    }

    deactivate_all_slots();
    return 0;
}

int bench_process()
{
    console() << "[Bench] ready for play\n";
    deactivate_all_slots();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        auto branches = make_branches(controller_process, bench_process);
        branches.back().auto_start = false;

        init(argc, argv, branches);

        if (process_index() == 0) {
            finalize();
        }
    }
    catch (const sintra::rpc_cancelled&) {
    }

    return 0;
}
