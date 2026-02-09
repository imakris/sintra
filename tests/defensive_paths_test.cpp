// Defensive paths test
// Exercises cleanup paths that are expected to be rarely hit in production.

#include <sintra/sintra.h>
#include <sintra/detail/process/coordinator.h>

#include "test_utils.h"

#include <exception>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view k_prefix = "defensive_paths_test: ";

bool test_null_barrier_cleanup()
{
    sintra::Process_group group;
    group.m_barriers["null_barrier"] = std::shared_ptr<sintra::Process_group::Barrier>();

    std::vector<sintra::Process_group::Barrier_completion> completions;
    group.drop_from_inflight_barriers(s_mproc_id, completions);

    return sintra::test::assert_true(group.m_barriers.empty(),
                                     k_prefix,
                                     "null barrier entry should be erased");
}

} // namespace

int main(int argc, char* argv[])
{
    bool ok = true;
    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& e) {
        sintra::test::print_test_message(k_prefix, e.what());
        return 1;
    }

    ok &= test_null_barrier_cleanup();

    sintra::finalize();
    return ok ? 0 : 1;
}
