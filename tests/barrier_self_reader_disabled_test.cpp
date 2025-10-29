#include <sintra/sintra.h>
#include "test_environment.h"

#include <sintra/detail/globals.h>
#include <sintra/detail/managed_process.h>
#include <sintra/detail/managed_process_impl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kEnvSharedDir = "SINTRA_SELF_READER_REPRO_DIR";

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("barrier_self_reader_disabled");
    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "self_reader_disabled_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

struct Result_record
{
    bool pending_before_cancel = false;
    bool barrier_succeeded = false;
};

void write_result(const std::filesystem::path& dir, const Result_record& record)
{
    std::ofstream out(dir / "result.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open result.txt for writing");
    }
    out << (record.pending_before_cancel ? 1 : 0) << '\n';
    out << (record.barrier_succeeded ? 1 : 0) << '\n';
}

std::optional<Result_record> read_result(const std::filesystem::path& dir)
{
    std::ifstream in(dir / "result.txt", std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Result_record record;
    int pending = 0;
    int success = 0;
    in >> pending;
    in >> success;
    if (!in) {
        return std::nullopt;
    }
    record.pending_before_cancel = (pending != 0);
    record.barrier_succeeded = (success != 0);
    return record;
}

int coordinator_process()
{
    sintra::barrier("self-reader-disabled/start", "_sintra_all_processes");
    s_mproc->test_disable_reader(sintra::process_of(s_coord_id));
    sintra::barrier("self-reader-disabled/ready", "_sintra_all_processes");
    std::this_thread::sleep_for(200ms);
    return 0;
}

int worker_process()
{
    sintra::barrier("self-reader-disabled/start", "_sintra_all_processes");
    sintra::barrier("self-reader-disabled/ready", "_sintra_all_processes");

    std::atomic<bool> completed{false};
    std::atomic<bool> success{false};

    std::thread barrier_thread([&] {
        try {
            bool result = sintra::barrier("self-reader-disabled/test", "_sintra_all_processes");
            success.store(result, std::memory_order_relaxed);
        }
        catch (const sintra::rpc_cancelled&) {
            success.store(false, std::memory_order_relaxed);
        }
        catch (...) {
            success.store(false, std::memory_order_relaxed);
        }
        completed.store(true, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(500ms);
    const bool pending = !completed.load(std::memory_order_relaxed);

    Result_record record;
    record.pending_before_cancel = pending;
    record.barrier_succeeded = false;

    auto dir = ensure_shared_directory();
    write_result(dir, record);

    if (pending) {
        s_mproc->unblock_rpc(sintra::process_of(s_coord_id));
    }

    if (barrier_thread.joinable()) {
        barrier_thread.join();
    }

    record.barrier_succeeded = success.load(std::memory_order_relaxed);
    write_result(dir, record);

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(worker_process);

    bool cancelled = false;
    try {
        sintra::init(argc, argv, processes);
        sintra::finalize();
    }
    catch (const sintra::rpc_cancelled&) {
        cancelled = true;
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "Main process exception: %s\n", ex.what());
        return 1;
    }

    if (!spawned) {
        std::optional<Result_record> result;
        for (int attempt = 0; attempt < 50; ++attempt) {
            result = read_result(shared_dir);
            if (result.has_value()) {
                break;
            }
            std::this_thread::sleep_for(100ms);
        }
        if (!result.has_value()) {
            return 0;
        }
        if (!result->pending_before_cancel) {
            std::fprintf(stderr, "Barrier completed without waiting\n");
            return 1;
        }
        if (result->barrier_succeeded) {
            std::fprintf(stderr, "Barrier unexpectedly succeeded\n");
            return 1;
        }
    }

    return 0;
}

