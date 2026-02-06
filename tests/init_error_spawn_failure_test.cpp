#include <sintra/sintra.h>

#include "test_utils.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int worker_entry()
{
    return 0;
}

std::vector<sintra::Process_descriptor> build_processes()
{
    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker_entry);
    processes.emplace_back(std::string("sintra_missing_binary__init_error_test__"));
    return processes;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    auto processes = build_processes();

    if (is_spawned) {
        sintra::init(argc, argv, processes);
        sintra::finalize();
        return 0;
    }

    bool caught = false;
    try {
        sintra::init(argc, argv, processes);
    }
    catch (const sintra::init_error& e) {
        caught = true;
        bool has_spawn_failed = false;
        for (const auto& failure : e.failures()) {
            if (failure.failure_cause == sintra::init_error::cause::spawn_failed) {
                has_spawn_failed = true;
                break;
            }
        }

        if (!has_spawn_failed) {
            std::fprintf(stderr, "Expected spawn_failed in init_error failures.\n");
            sintra::finalize();
            return 1;
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Unexpected exception: %s\n", e.what());
        sintra::finalize();
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "Unexpected non-std exception.\n");
        sintra::finalize();
        return 1;
    }

    if (!caught) {
        std::fprintf(stderr, "Expected sintra::init to throw init_error.\n");
        sintra::finalize();
        return 1;
    }

    sintra::finalize();
    return 0;
}
