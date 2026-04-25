//
// Sintra rpc_unavailable typed-exception round-trip test
//
// Verifies that when a remote RPC handler throws sintra::rpc_unavailable,
// the caller catches it as a typed sintra::rpc_unavailable (not as a plain
// std::runtime_error). Without the dedicated catch arm in the RPC handler
// exception serialization path, the exception would be downgraded to
// std_runtime_error on the wire and lose its typed identity.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char* k_unavailable_message = "synthetic rpc_unavailable from handler";

struct Throwing_service : sintra::Derived_transceiver<Throwing_service>
{
    int provoke_unavailable(int /*ignored*/)
    {
        throw sintra::rpc_unavailable(k_unavailable_message);
    }

    SINTRA_RPC(provoke_unavailable)
};

int process_owner()
{
    Throwing_service service;
    service.assign_name("rpc_unavailable_typed_test_owner");

    sintra::barrier("service-ready");
    sintra::barrier("calls-finished", "_sintra_all_processes");
    return 0;
}

int process_client()
{
    sintra::barrier("service-ready");

    std::vector<std::string> outcomes;

    try {
        (void)Throwing_service::rpc_provoke_unavailable(
            "rpc_unavailable_typed_test_owner", 7);
        outcomes.push_back("unexpected-success");
    }
    catch (const sintra::rpc_unavailable& e) {
        outcomes.push_back(std::string("typed:") + e.what());
    }
    catch (const std::runtime_error& e) {
        // The typed exception derives from std::runtime_error, so a missing
        // typed catch would land here. Distinguish that case explicitly.
        outcomes.push_back(std::string("runtime_error:") + e.what());
    }
    catch (const std::exception& e) {
        outcomes.push_back(std::string("other:") + e.what());
    }

    const sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR", "rpc_unavailable_typed");
    sintra::test::write_lines(shared.path() / "outcomes.txt", outcomes);

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
        "rpc_unavailable_typed",
        {process_owner, process_client},
        [](const std::filesystem::path&) {
            sintra::barrier("calls-finished", "_sintra_all_processes");
            return 0;
        },
        [](const std::filesystem::path& shared_dir) {
            const auto outcomes =
                sintra::test::read_lines(shared_dir / "outcomes.txt");
            const std::vector<std::string> expected = {
                std::string("typed:") + k_unavailable_message,
            };
            return outcomes == expected ? 0 : 1;
        });
}
