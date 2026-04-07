#include <sintra/sintra.h>

#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string value;
    std::getline(in, value);
    return value;
}

int processing_worker()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "barrier_guardrails");
    const auto result_path = shared.path() / "processing_worker.txt";

    sintra::barrier("mixed-mode-ready", "_sintra_external_processes");

    try {
        const auto sequence =
            sintra::barrier<sintra::processing_fence_t>("mixed-mode", "_sintra_external_processes");
        write_text(
            result_path,
            sintra::barrier_completed(sequence) ? "completed" : "bypassed");
        return 0;
    }
    catch (const std::logic_error& e) {
        write_text(result_path, std::string("logic_error:") + e.what());
        return 0;
    }
    catch (const std::exception& e) {
        write_text(result_path, std::string("unexpected:") + e.what());
        return 1;
    }
}

int rendezvous_worker()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "barrier_guardrails");
    const auto result_path = shared.path() / "rendezvous_worker.txt";

    sintra::barrier("mixed-mode-ready", "_sintra_external_processes");

    try {
        const auto sequence =
            sintra::barrier<sintra::rendezvous_t>("mixed-mode", "_sintra_external_processes");
        write_text(
            result_path,
            sintra::barrier_completed(sequence) ? "completed" : "bypassed");
        return 0;
    }
    catch (const std::logic_error& e) {
        write_text(result_path, std::string("logic_error:") + e.what());
        return 0;
    }
    catch (const std::exception& e) {
        write_text(result_path, std::string("unexpected:") + e.what());
        return 1;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    return sintra::test::run_multi_process_test_raw(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "barrier_guardrails",
        {processing_worker, rendezvous_worker},
        [](const std::filesystem::path& shared_dir) {
            write_text(shared_dir / "reserved_name_result.txt", "");
        },
        [](const std::filesystem::path& shared_dir) {
            try {
                (void)sintra::barrier("_sintra_reserved_name");
                write_text(shared_dir / "reserved_name_result.txt", "no-throw");
                return 1;
            }
            catch (const std::invalid_argument&) {
                write_text(shared_dir / "reserved_name_result.txt", "ok");
                return 0;
            }
            catch (const std::exception& e) {
                write_text(
                    shared_dir / "reserved_name_result.txt",
                    std::string("unexpected:") + e.what());
                return 1;
            }
        },
        [](const std::filesystem::path& shared_dir) {
            const auto processing_result = read_text(shared_dir / "processing_worker.txt");
            const auto rendezvous_result = read_text(shared_dir / "rendezvous_worker.txt");
            const auto reserved_name_result = read_text(shared_dir / "reserved_name_result.txt");

            const bool reserved_name_ok = (reserved_name_result == "ok");
            const bool mixed_mode_flagged =
                (processing_result.rfind("logic_error:Barrier mode mismatch", 0) == 0) ||
                (rendezvous_result.rfind("logic_error:Barrier mode mismatch", 0) == 0);
            const bool no_unexpected_error =
                (processing_result.rfind("unexpected:", 0) != 0) &&
                (rendezvous_result.rfind("unexpected:", 0) != 0);

            return (reserved_name_ok && mixed_mode_flagged && no_unexpected_error) ? 0 : 1;
        });
}
