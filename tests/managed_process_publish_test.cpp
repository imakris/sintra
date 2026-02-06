//
// Sintra Managed Process Publish Test
//
// Launches a process manually (without spawn_swarm_process) and verifies the
// coordinator accepts the Managed_process publish path when the registry entry
// does not yet exist.
//

#include <sintra/sintra.h>
#include <sintra/detail/utility.h>
#include <sintra/detail/messaging/process_message_reader.h>
#include <sintra/detail/process/dispatch_wait_guard.h>

#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view k_worker_flag = "--managed_process_publish_worker";
constexpr std::string_view k_dir_flag = "--managed_process_publish_dir";

constexpr std::string_view k_delayed_worker_flag = "--managed_process_publish_delayed_worker";
constexpr std::string_view k_delayed_dir_flag = "--managed_process_publish_delayed_dir";
constexpr std::string_view k_delayed_role_flag = "--managed_process_publish_role";

constexpr std::string_view k_info_file = "managed_process_publish_info.txt";
constexpr std::string_view k_done_file = "managed_process_publish_done.txt";
constexpr std::string_view k_exit_file = "managed_process_publish_exit.txt";

constexpr std::string_view k_delayed_b_name_file = "managed_process_publish_delayed_b_name.txt";
constexpr std::string_view k_delayed_b_marked_file = "managed_process_publish_delayed_b_marked.txt";
constexpr std::string_view k_delayed_done_file = "managed_process_publish_delayed_done.txt";
constexpr std::string_view k_delayed_exit_file = "managed_process_publish_delayed_exit.txt";

bool has_flag(int argc, char* argv[], std::string_view flag)
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == flag) {
            return true;
        }
    }
    return false;
}

std::string read_flag_value(int argc, char* argv[], std::string_view flag)
{
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == flag) {
            return std::string(argv[i + 1]);
        }
    }
    return {};
}

struct Worker_info
{
    sintra::instance_id_type instance_id = sintra::invalid_instance_id;
    std::string assigned_name;
};

bool write_worker_info(const std::filesystem::path& dir, const Worker_info& info)
{
    std::ofstream out(dir / std::string(k_info_file), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    out << static_cast<unsigned long long>(info.instance_id) << '\n';
    out << info.assigned_name << '\n';
    return static_cast<bool>(out);
}

std::optional<Worker_info> read_worker_info(const std::filesystem::path& dir)
{
    std::ifstream in(dir / std::string(k_info_file), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    Worker_info info{};
    unsigned long long iid = 0;
    if (!(in >> iid)) {
        return std::nullopt;
    }

    std::string name;
    std::getline(in >> std::ws, name);
    if (name.empty()) {
        return std::nullopt;
    }

    info.instance_id = static_cast<sintra::instance_id_type>(iid);
    info.assigned_name = std::move(name);
    return info;
}

bool wait_for_path(const std::filesystem::path& path, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::filesystem::exists(path);
}

struct Publication_waiter
{
    std::mutex mutex;
    std::condition_variable cv;
    sintra::instance_id_type expected_instance_id = sintra::invalid_instance_id;
    bool expected_ready = false;
    bool seen = false;
};

bool run_drain_timeout_probe()
{
    if (!s_coord) {
        std::fprintf(stderr, "managed_process_publish_test: coordinator missing\n");
        return false;
    }

    s_coord->set_drain_timeout(std::chrono::seconds(1));
    const bool drained = s_coord->wait_for_all_draining(s_mproc_id);
    s_coord->set_drain_timeout(std::chrono::seconds(20));

    if (drained) {
        std::fprintf(stderr, "managed_process_publish_test: expected drain timeout, got success\n");
        return false;
    }

    return true;
}

int run_worker(int argc, char* argv[])
{
    const std::string dir_value = read_flag_value(argc, argv, k_dir_flag);
    if (dir_value.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: missing %s\n", k_dir_flag.data());
        return 1;
    }

    const std::filesystem::path shared_dir(dir_value);
    sintra::init(argc, argv);

    Worker_info info{};
    info.instance_id = s_mproc_id;
    info.assigned_name = std::string("sintra_process_") + std::to_string(
        static_cast<unsigned long long>(s_mproc->m_pid));

    if (!write_worker_info(shared_dir, info)) {
        std::fprintf(stderr, "managed_process_publish_test: failed to write worker info\n");
        sintra::finalize();
        return 1;
    }

    const auto done_path = shared_dir / std::string(k_done_file);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !std::filesystem::exists(done_path)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const bool done = std::filesystem::exists(done_path);
    if (!done) {
        std::fprintf(stderr, "managed_process_publish_test: timed out waiting for done signal\n");
        sintra::finalize();
        return 1;
    }

    std::ofstream exit_marker(shared_dir / std::string(k_exit_file), std::ios::binary | std::ios::trunc);
    exit_marker << "exit\n";

    sintra::finalize();
    return 0;
}

int run_delayed_publication_worker(int argc, char* argv[])
{
    const std::string dir_value = read_flag_value(argc, argv, k_delayed_dir_flag);
    if (dir_value.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: missing %s\n", k_delayed_dir_flag.data());
        return 1;
    }

    const std::string role = read_flag_value(argc, argv, k_delayed_role_flag);
    if (role.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: missing %s\n", k_delayed_role_flag.data());
        return 1;
    }

    const std::filesystem::path shared_dir(dir_value);
    sintra::init(argc, argv);

    const auto marked_path = shared_dir / std::string(k_delayed_b_marked_file);
    const auto done_path = shared_dir / std::string(k_delayed_done_file);

    if (role == "b") {
        const std::string assigned_name = std::string("sintra_process_") + std::to_string(
            static_cast<unsigned long long>(s_mproc->m_pid));

        std::ofstream name_file(shared_dir / std::string(k_delayed_b_name_file),
                                std::ios::binary | std::ios::trunc);
        name_file << assigned_name << '\n';
        name_file.close();

        sintra::Coordinator::rpc_mark_initialization_complete(s_coord_id, s_mproc_id);

        std::ofstream marked_file(marked_path, std::ios::binary | std::ios::trunc);
        marked_file << "marked\n";
        marked_file.close();

        if (!wait_for_path(done_path, std::chrono::seconds(10))) {
            std::fprintf(stderr, "managed_process_publish_test: delayed worker timed out\n");
            sintra::finalize();
            return 1;
        }

        std::ofstream exit_file(shared_dir / std::string(k_delayed_exit_file),
                                std::ios::binary | std::ios::trunc);
        exit_file << "exit\n";
        exit_file.close();
    }
    else if (role == "a") {
        if (!wait_for_path(marked_path, std::chrono::seconds(10))) {
            std::fprintf(stderr, "managed_process_publish_test: delayed worker missing mark\n");
            sintra::finalize();
            return 1;
        }
    }
    else {
        std::fprintf(stderr, "managed_process_publish_test: unknown delayed role '%s'\n", role.c_str());
        sintra::finalize();
        return 1;
    }

    sintra::finalize();
    return 0;
}

bool spawn_worker_process(
    const std::string& binary_path,
    const std::filesystem::path& shared_dir,
    uint64_t swarm_id,
    sintra::instance_id_type instance_id,
    sintra::instance_id_type coordinator_id)
{
    std::vector<std::string> args;
    args.push_back(binary_path);
    args.push_back(std::string(k_worker_flag));
    args.push_back(std::string(k_dir_flag));
    args.push_back(shared_dir.string());
    args.push_back("--swarm_id");
    args.push_back(std::to_string(swarm_id));
    args.push_back("--instance_id");
    args.push_back(std::to_string(instance_id));
    args.push_back("--coordinator_id");
    args.push_back(std::to_string(coordinator_id));
    args.push_back("--lifeline_disable");

    sintra::cstring_vector cargs(std::move(args));

    sintra::Spawn_detached_options options;
    options.prog = binary_path.c_str();
    options.argv = cargs.v();
    return sintra::spawn_detached(options);
}

bool ensure_reader_for_process(sintra::instance_id_type process_iid)
{
    if (!s_mproc) {
        return false;
    }

    sintra::Dispatch_lock_guard<std::unique_lock<std::shared_mutex>> readers_lock(s_mproc->m_readers_mutex);
    if (auto existing = s_mproc->m_readers.find(process_iid); existing != s_mproc->m_readers.end()) {
        if (existing->second) {
            existing->second->stop_and_wait(1.0);
        }
        s_mproc->m_readers.erase(existing);
    }

    auto progress = std::make_shared<sintra::Process_message_reader::Delivery_progress>();
    auto reader = std::make_shared<sintra::Process_message_reader>(process_iid, progress, 0u);
    auto [it, inserted] = s_mproc->m_readers.emplace(process_iid, reader);
    if (!inserted) {
        return false;
    }

    reader->wait_until_ready();
    return true;
}

bool spawn_delayed_worker(
    const std::string& binary_path,
    const std::filesystem::path& shared_dir,
    const char* role,
    uint64_t swarm_id,
    sintra::instance_id_type process_instance_id,
    sintra::instance_id_type coordinator_id)
{
    std::vector<std::string> args;
    args.push_back(binary_path);
    args.push_back(std::string(k_delayed_worker_flag));
    args.push_back(std::string(k_delayed_dir_flag));
    args.push_back(shared_dir.string());
    args.push_back(std::string(k_delayed_role_flag));
    args.push_back(role);
    args.push_back("--swarm_id");
    args.push_back(std::to_string(swarm_id));
    args.push_back("--instance_id");
    args.push_back(std::to_string(process_instance_id));
    args.push_back("--coordinator_id");
    args.push_back(std::to_string(coordinator_id));
    args.push_back("--lifeline_disable");

    sintra::cstring_vector cargs(std::move(args));

    sintra::Spawn_detached_options options;
    options.prog = binary_path.c_str();
    options.argv = cargs.v();
    return sintra::spawn_detached(options);
}

bool run_manual_publish_scenario(
    const std::string& binary_path,
    const std::filesystem::path& shared_dir)
{
    const auto worker_instance_id = sintra::make_process_instance_id();
    const auto swarm_id = s_mproc->m_swarm_id;
    const auto coord_id = s_coord_id;

    if (!ensure_reader_for_process(worker_instance_id)) {
        std::fprintf(stderr, "managed_process_publish_test: failed to create reader for worker\n");
        return false;
    }

    if (!spawn_worker_process(binary_path, shared_dir, swarm_id, worker_instance_id, coord_id)) {
        std::fprintf(stderr, "managed_process_publish_test: failed to spawn worker process\n");
        return false;
    }

    const auto info_path = shared_dir / std::string(k_info_file);
    if (!wait_for_path(info_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: worker info file not found\n");
        return false;
    }

    const auto info = read_worker_info(shared_dir);
    if (!info || info->assigned_name.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: failed to read worker info\n");
        return false;
    }

    bool resolved_ok = false;
    const auto resolve_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < resolve_deadline) {
        const auto resolved = sintra::Coordinator::rpc_resolve_instance(coord_id, info->assigned_name);
        if (resolved == info->instance_id) {
            resolved_ok = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!resolved_ok) {
        std::fprintf(stderr,
                     "managed_process_publish_test: name '%s' did not resolve to %llu\n",
                     info->assigned_name.c_str(),
                     static_cast<unsigned long long>(info->instance_id));
    }

    std::ofstream done(shared_dir / std::string(k_done_file), std::ios::binary | std::ios::trunc);
    done << "done\n";
    done.close();

    const auto exit_path = shared_dir / std::string(k_exit_file);
    if (!wait_for_path(exit_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: worker did not confirm exit\n");
        resolved_ok = false;
    }

    return resolved_ok;
}

bool run_delayed_publication_scenario(const std::string& binary_path)
{
    Publication_waiter waiter;
    const auto shared_dir = sintra::test::unique_scratch_directory("managed_process_publish_delayed");
    std::filesystem::create_directories(shared_dir);

    const auto swarm_id = s_mproc->m_swarm_id;
    const auto coord_id = s_coord_id;

    auto handler = [&waiter](const sintra::Coordinator::instance_published& msg) {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        if (!waiter.expected_ready) {
            return;
        }
        if (msg.instance_id == waiter.expected_instance_id) {
            waiter.seen = true;
            waiter.cv.notify_one();
        }
    };
    sintra::activate_slot(handler, sintra::Typed_instance_id<sintra::Coordinator>(s_coord_id));

    const auto process_a = sintra::make_process_instance_id();
    const auto process_b = sintra::make_process_instance_id();
    {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        waiter.expected_instance_id = process_b;
        waiter.expected_ready = true;
    }

    if (!ensure_reader_for_process(process_a) || !ensure_reader_for_process(process_b)) {
        std::fprintf(stderr, "managed_process_publish_test: delayed readers not ready\n");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_coord->m_init_tracking_mutex);
        s_coord->m_processes_in_initialization.insert(process_a);
        s_coord->m_processes_in_initialization.insert(process_b);
    }

    if (!spawn_delayed_worker(binary_path, shared_dir, "a", swarm_id, process_a, coord_id) ||
        !spawn_delayed_worker(binary_path, shared_dir, "b", swarm_id, process_b, coord_id))
    {
        std::fprintf(stderr, "managed_process_publish_test: delayed spawn failed\n");
        return false;
    }

    const auto name_path = shared_dir / std::string(k_delayed_b_name_file);
    if (!wait_for_path(name_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: delayed name file missing\n");
        return false;
    }

    std::ifstream name_in(name_path, std::ios::binary);
    std::string expected_name;
    std::getline(name_in >> std::ws, expected_name);
    if (expected_name.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: delayed name file empty\n");
        return false;
    }

    const auto marked_path = shared_dir / std::string(k_delayed_b_marked_file);
    if (!wait_for_path(marked_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: delayed mark file missing\n");
        return false;
    }

    bool published = false;
    {
        std::unique_lock<std::mutex> lock(waiter.mutex);
        published = waiter.cv.wait_for(lock, std::chrono::seconds(5), [&]() {
            return waiter.seen;
        });
    }

    if (!published) {
        std::fprintf(stderr, "managed_process_publish_test: delayed publication not observed\n");
    }

    std::ofstream done(shared_dir / std::string(k_delayed_done_file),
                       std::ios::binary | std::ios::trunc);
    done << "done\n";
    done.close();

    const auto exit_path = shared_dir / std::string(k_delayed_exit_file);
    if (!wait_for_path(exit_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: delayed worker did not exit\n");
        published = false;
    }

    std::error_code ec;
    std::filesystem::remove_all(shared_dir, ec);

    return published;
}

} // namespace

int main(int argc, char* argv[])
{
    if (has_flag(argc, argv, k_worker_flag)) {
        return run_worker(argc, argv);
    }
    if (has_flag(argc, argv, k_delayed_worker_flag)) {
        return run_delayed_publication_worker(argc, argv);
    }

    const auto shared_dir = sintra::test::unique_scratch_directory("managed_process_publish");
    std::filesystem::create_directories(shared_dir);

    const std::string binary_path = (argc > 0 && argv[0]) ? std::string(argv[0]) : std::string();
    if (binary_path.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: missing binary path\n");
        return 1;
    }

    sintra::init(argc, argv);

    bool ok = true;
    if (!run_drain_timeout_probe()) {
        ok = false;
    }

    if (ok) {
        ok = run_manual_publish_scenario(binary_path, shared_dir);
    }

    if (ok) {
        ok = run_delayed_publication_scenario(binary_path);
    }

    sintra::finalize();

    std::error_code ec;
    std::filesystem::remove_all(shared_dir, ec);

    return ok ? 0 : 1;
}
