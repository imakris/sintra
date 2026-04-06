//
// Sintra RPC Append Test
//
// This test validates Remote Procedure Call (RPC) functionality of Sintra.
// It corresponds to example_2 and tests the following features:
// - Transceiver derivatives with exported RPC methods
// - Named instances accessible across processes
// - RPC calls using instance names
// - Exception propagation across process boundaries
// - Successful and failed RPC invocations
//
// Test structure:
// - Process 1 (owner): Creates and names a Remotely_accessible object
// - Process 2 (client): Makes RPC calls to the named object
//
// The test verifies that:
// - Successful RPC calls return correct values
// - RPC calls that trigger exceptions propagate the exception correctly
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Remotely_accessible : sintra::Derived_transceiver<Remotely_accessible> {
    std::string append(const std::string& s, int v)
    {
        if (s.size() > 10) {
            throw std::logic_error("string too long");
        }
        return std::to_string(v) + ": " + s;
    }

    SINTRA_RPC(append)
};

int process_owner()
{
    Remotely_accessible ra;
    ra.assign_name("instance name");

    sintra::barrier("object-ready");
    sintra::barrier("calls-finished", "_sintra_all_processes");
    return 0;
}

int process_client()
{
    sintra::barrier("object-ready");

    struct Test_case {
        std::string city;
        int year;
        bool expect_success;
    };

    const std::vector<Test_case> cases = {
        {"Sydney", 2000, true},
        {"Athens", 2004, true},
        {"Beijing", 2008, true},
        {"Rio de Janeiro", 2016, false}, // triggers "string too long"
    };

    std::vector<std::string> successes;
    std::vector<std::string> failures;

    for (const auto& tc : cases) {
        try {
            auto value = Remotely_accessible::rpc_append("instance name", tc.city, tc.year);
            successes.push_back(value);
        }
        catch (const std::exception& e) {
            failures.push_back(e.what());
        }
    }

    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "rpc_append");
    sintra::test::write_lines(shared.path() / "rpc_success.txt", successes);
    sintra::test::write_lines(shared.path() / "rpc_failures.txt", failures);

    sintra::barrier("calls-finished", "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "rpc_append",
        {process_owner, process_client},
        [](const std::filesystem::path& shared_dir) {
            const auto success_path = shared_dir / "rpc_success.txt";
            const auto failure_path = shared_dir / "rpc_failures.txt";

            const auto successes = sintra::test::read_lines(success_path);
            const auto failures = sintra::test::read_lines(failure_path);

            const std::vector<std::string> expected_successes = {
                "2000: Sydney",
                "2004: Athens",
                "2008: Beijing",
            };
            const std::vector<std::string> expected_failures = {
                "string too long",
            };

            return (successes == expected_successes && failures == expected_failures) ? 0 : 1;
        },
        "calls-finished");
}
