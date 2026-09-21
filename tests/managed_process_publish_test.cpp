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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using sintra::s_mproc;
using sintra::s_mproc_id;
using sintra::s_coord;
using sintra::s_coord_id;

namespace {

constexpr std::string_view k_worker_flag = "--managed_process_publish_worker";
constexpr std::string_view k_dir_flag    = "--managed_process_publish_dir";

constexpr std::string_view k_delayed_worker_flag = "--managed_process_publish_delayed_worker";
constexpr std::string_view k_delayed_dir_flag    = "--managed_process_publish_delayed_dir";
constexpr std::string_view k_delayed_role_flag   = "--managed_process_publish_role";

constexpr std::string_view k_info_file = "managed_process_publish_info.txt";
constexpr std::string_view k_done_file = "managed_process_publish_done.txt";
constexpr std::string_view k_exit_file = "managed_process_publish_exit.txt";

constexpr std::string_view k_delayed_b_name_file   = "managed_process_publish_delayed_b_name.txt";
constexpr std::string_view k_delayed_b_marked_file = "managed_process_publish_delayed_b_marked.txt";
constexpr std::string_view k_delayed_done_file     = "managed_process_publish_delayed_done.txt";
constexpr std::string_view k_delayed_exit_file     = "managed_process_publish_delayed_exit.txt";

struct Worker_info
{
    sintra::instance_id_type   instance_id = sintra::invalid_instance_id;
    std::string                assigned_name;
};

bool write_worker_info(const std::filesystem::path& dir, const Worker_info& info)
{
    const auto path = dir / std::string(k_info_file);
    const auto tmp_path = path.string() + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    out << static_cast<unsigned long long>(info.instance_id) << '\n';
    out << info.assigned_name << '\n';
    out.close();
    if (!out) {
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    return !ec;
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

struct Publication_waiter
{
    std::mutex                 mutex;
    std::condition_variable    cv;
    sintra::instance_id_type   expected_instance_id = sintra::invalid_instance_id;
    bool                       expected_ready       = false;
    bool                       seen                 = false;
};

struct Local_publication_object : sintra::Derived_transceiver<Local_publication_object> {};

struct Publication_reader : sintra::Derived_transceiver<Publication_reader>
{
    std::promise<void>         entered;
    std::shared_future<void>   release;

    int hold()
    {
        entered.set_value();
        if (release.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("publication reader release timed out");
        }
        return 17;
    }

    int fail()
    {
        throw std::runtime_error("publication transport sentinel");
    }

    SINTRA_RPC_STRICT(hold)
    SINTRA_RPC_STRICT(fail)
};

bool run_held_reader_publication()
{
    Local_publication_object object;
    Publication_reader reader;
    std::promise<void> release;
    reader.release = release.get_future().share();
    auto entered = reader.entered.get_future();
    std::future<bool> publication;

    // Release before destroying the publication future or the reader, including
    // exception/failed-setup paths. The reader also has its own bounded wait.
    struct Release_reader
    {
        std::promise<void>& promise;
        bool released = false;

        void finish()
        {
            if (!released) {
                promise.set_value();
                released = true;
            }
        }

        ~Release_reader() { finish(); }
    } release_reader{release};

    try {
        const std::string name = "managed_process_publish/held_reader";
        auto waiter = sintra::Coordinator::rpc_async_wait_for_instance(s_coord_id, name);
        auto held = Publication_reader::rpc_async_hold(reader.instance_id());
        if (entered.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
            std::fprintf(stderr, "managed_process_publish_test: reader did not enter hold\n");
            return false;
        }
        publication = std::async(std::launch::async, [&, name] { return object.assign_name(name); });
        const bool completed_while_held =
            publication.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
        release_reader.finish();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        const bool held_result = held.get_until(deadline) == 17;
        const bool published = publication.get();
        const bool exact_waiter = waiter.get_until(deadline) == object.instance_id();
        bool exact_exception = false;
        try {
            auto failed = Publication_reader::rpc_async_fail(reader.instance_id());
            (void)failed.get_until(std::chrono::steady_clock::now() + std::chrono::seconds(3));
        }
        catch (const std::runtime_error& error) {
            exact_exception = std::string_view(error.what()) == "publication transport sentinel";
        }
        std::fprintf(stderr,
            "managed_process_publish_test: held_reader publication=%d hold=%d published=%d waiter=%d exception=%d\n",
            completed_while_held, held_result, published, exact_waiter, exact_exception);
        return completed_while_held && held_result && published && exact_waiter && exact_exception;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_process_publish_test: held-reader exception: %s\n", error.what());
        return false;
    }
}

bool publication_guard_observed = false;

void throw_from_publication_stage(const char* stage)
{
    if (std::string_view(stage) !=
        sintra::detail::test_hooks::k_stage_publish_transceiver_locked)
    {
        return;
    }
    const auto* targets = sintra::detail::tl_executing_rpc_targets;
    publication_guard_observed = targets &&
        std::find(targets->begin(), targets->end(), s_coord) != targets->end();
    // Only observe the execution guard; never re-enter coordinator APIs here.
    throw std::runtime_error("publication unwind sentinel");
}

bool run_local_publication_contract()
{
    Local_publication_object object;
    Local_publication_object duplicate;
    Local_publication_object unwound;
    const auto sentinel = object.instance_id();

    struct Restore_reply_state
    {
        sintra::instance_id_type common = sintra::s_tl_common_function_iid;
        std::vector<sintra::instance_id_type> recipients{
            sintra::s_tl_additional_piids,
            sintra::s_tl_additional_piids + sintra::s_tl_additional_piids_size};
        sintra::detail::test_hooks::Coordinator_lock_stage_callback hook =
            sintra::detail::test_hooks::s_coordinator_lock_stage.load();

        ~Restore_reply_state()
        {
            sintra::detail::test_hooks::s_coordinator_lock_stage.store(hook);
            std::copy(recipients.begin(), recipients.end(), sintra::s_tl_additional_piids);
            sintra::s_tl_additional_piids_size = recipients.size();
            sintra::s_tl_common_function_iid = common;
        }
    } restore;

    sintra::s_tl_common_function_iid = sentinel;
    sintra::s_tl_additional_piids[0] = s_mproc_id;
    sintra::s_tl_additional_piids_size = 1;
    const auto state_preserved = [&] {
        return sintra::s_tl_common_function_iid == sentinel &&
            sintra::s_tl_additional_piids_size == 1 &&
            sintra::s_tl_additional_piids[0] == s_mproc_id;
    };

    try {
        const bool published = object.assign_name("managed_process_publish/local_contract");
        const bool success_tls = state_preserved();
        const bool duplicate_rejected = !duplicate.assign_name("managed_process_publish/local_contract");
        const bool duplicate_tls = state_preserved();
        const bool empty_rejected = !duplicate.assign_name("");
        const bool empty_tls = state_preserved();

        publication_guard_observed = false;
        sintra::detail::test_hooks::s_coordinator_lock_stage.store(throw_from_publication_stage);
        bool exact_exception = false;
        try {
            (void)unwound.assign_name("managed_process_publish/unwind");
        }
        catch (const std::runtime_error& error) {
            exact_exception = std::string_view(error.what()) == "publication unwind sentinel";
        }
        sintra::detail::test_hooks::s_coordinator_lock_stage.store(restore.hook);
        const bool unwind_tls = state_preserved();
        const auto* targets = sintra::detail::tl_executing_rpc_targets;
        const bool guard_released = !targets || targets->empty();
        const bool retry = unwound.assign_name("managed_process_publish/unwind");
        const bool retry_tls = state_preserved();
        const bool tls = success_tls && duplicate_tls && empty_tls && unwind_tls && retry_tls;
        std::fprintf(stderr,
            "managed_process_publish_test: local_contract published=%d duplicate=%d empty=%d tls=%d guard=%d exception=%d released=%d retry=%d\n",
            published, duplicate_rejected, empty_rejected, tls, publication_guard_observed,
            exact_exception, guard_released, retry);
        return published && duplicate_rejected && empty_rejected && tls &&
            publication_guard_observed && exact_exception && guard_released && retry;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_process_publish_test: local-contract exception: %s\n", error.what());
        return false;
    }
}

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
    const std::string dir_value = sintra::test::get_argv_value(argc, argv, k_dir_flag);
    if (dir_value.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: missing %s\n", k_dir_flag.data());
        return 1;
    }

    const std::filesystem::path shared_dir(dir_value);
    sintra::init(argc, argv);

    Worker_info info{};
    info.instance_id = s_mproc_id;
    info.assigned_name = std::string(
        "sintra_process_") + std::to_string(static_cast<unsigned long long>(s_mproc->m_pid));

    if (!write_worker_info(shared_dir, info)) {
        std::fprintf(stderr, "managed_process_publish_test: failed to write worker info\n");
        sintra::detail::finalize();
        return 1;
    }

    const auto done_path = shared_dir / std::string(k_done_file);
    const bool done = sintra::test::wait_for_file(
        done_path,
        std::chrono::seconds(10),
        std::chrono::milliseconds(20));
    if (!done) {
        std::fprintf(stderr, "managed_process_publish_test: timed out waiting for done signal\n");
        sintra::detail::finalize();
        return 1;
    }

    std::ofstream exit_marker(
        shared_dir / std::string(k_exit_file),
        std::ios::binary | std::ios::trunc);
    exit_marker << "exit\n";

    sintra::detail::finalize();
    return 0;
}

int run_delayed_publication_worker(int argc, char* argv[])
{
    const std::string dir_value = sintra::test::get_argv_value(argc, argv, k_delayed_dir_flag);
    if (dir_value.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: missing %s\n", k_delayed_dir_flag.data());
        return 1;
    }

    const std::string role = sintra::test::get_argv_value(argc, argv, k_delayed_role_flag);
    if (role.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: missing %s\n", k_delayed_role_flag.data());
        return 1;
    }

    const std::filesystem::path shared_dir(dir_value);
    sintra::init(argc, argv);

    const auto marked_path = shared_dir / std::string(k_delayed_b_marked_file);
    const auto done_path   = shared_dir / std::string(k_delayed_done_file);

    if (role == "b") {
        const std::string assigned_name = std::string(
            "sintra_process_") + std::to_string(static_cast<unsigned long long>(s_mproc->m_pid));

        std::ofstream name_file(
            shared_dir / std::string(k_delayed_b_name_file),
            std::ios::binary | std::ios::trunc);
        name_file << assigned_name << '\n';
        name_file.close();

        sintra::Coordinator::rpc_mark_initialization_complete(s_coord_id, s_mproc_id);

        std::ofstream marked_file(marked_path, std::ios::binary | std::ios::trunc);
        marked_file << "marked\n";
        marked_file.close();

        if (!sintra::test::wait_for_file(done_path, std::chrono::seconds(10))) {
            std::fprintf(stderr, "managed_process_publish_test: delayed worker timed out\n");
            sintra::detail::finalize();
            return 1;
        }

        std::ofstream exit_file(
            shared_dir / std::string(k_delayed_exit_file),
            std::ios::binary | std::ios::trunc);
        exit_file << "exit\n";
        exit_file.close();
    }
    else
    if (role == "a") {
        if (!sintra::test::wait_for_file(marked_path, std::chrono::seconds(10))) {
            std::fprintf(stderr, "managed_process_publish_test: delayed worker missing mark\n");
            sintra::detail::finalize();
            return 1;
        }
    }
    else {
        std::fprintf(stderr, "managed_process_publish_test: unknown delayed role '%s'\n", role.c_str());
        sintra::detail::finalize();
        return 1;
    }

    sintra::detail::finalize();
    return 0;
}

bool spawn_worker_process(
    const std::string&             binary_path,
    const std::filesystem::path&   shared_dir,
    uint64_t                       swarm_id,
    sintra::instance_id_type       instance_id,
    sintra::instance_id_type       coordinator_id)
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

    sintra::C_string_vector cargs(std::move(args));

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

    sintra::Dispatch_lock_guard<std::unique_lock<sintra::shared_mutex>> readers_lock(s_mproc->m_readers_mutex);
    if (auto existing = s_mproc->m_readers.find(process_iid); existing != s_mproc->m_readers.end()) {
        if (existing->second) {
            existing->second->stop_and_wait(1.0);
        }
        s_mproc->m_readers.erase(existing);
    }

    auto progress = std::make_shared<sintra::Process_message_reader::Delivery_progress>();
    auto reader   = std::make_shared<sintra::Process_message_reader>(process_iid, progress, 0u);
    auto [it, inserted] = s_mproc->m_readers.emplace(process_iid, reader);
    if (!inserted) {
        return false;
    }

    reader->wait_until_ready();
    return true;
}

bool spawn_delayed_worker(
    const std::string&             binary_path,
    const std::filesystem::path&   shared_dir,
    const char*                    role,
    uint64_t                       swarm_id,
    sintra::instance_id_type       process_instance_id,
    sintra::instance_id_type       coordinator_id)
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

    sintra::C_string_vector cargs(std::move(args));

    sintra::Spawn_detached_options options;
    options.prog = binary_path.c_str();
    options.argv = cargs.v();
    return sintra::spawn_detached(options);
}

bool run_manual_publish_scenario(
    const std::string&             binary_path,
    const std::filesystem::path&   shared_dir)
{
    const auto worker_instance_id = sintra::make_process_instance_id();
    const auto swarm_id           = s_mproc->m_swarm_id;
    const auto coord_id           = s_coord_id;

    if (!ensure_reader_for_process(worker_instance_id)) {
        std::fprintf(stderr, "managed_process_publish_test: failed to create reader for worker\n");
        return false;
    }

    if (!spawn_worker_process(binary_path, shared_dir, swarm_id, worker_instance_id, coord_id)) {
        std::fprintf(stderr, "managed_process_publish_test: failed to spawn worker process\n");
        return false;
    }

    const auto release_worker = [&]() {
        std::ofstream done(
            shared_dir / std::string(k_done_file),
            std::ios::binary | std::ios::trunc);
        done << "done\n";
    };

    const auto info_path = shared_dir / std::string(k_info_file);
    if (!sintra::test::wait_for_file(info_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: worker info file not found\n");
        release_worker();
        return false;
    }

    const auto info = read_worker_info(shared_dir);
    if (!info || info->assigned_name.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: failed to read worker info\n");
        release_worker();
        return false;
    }

    bool       resolved_ok      = false;
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

    release_worker();

    const auto exit_path = shared_dir / std::string(k_exit_file);
    if (!sintra::test::wait_for_file(exit_path, std::chrono::seconds(5))) {
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
    auto deactivate_handler =
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
        deactivate_handler();
        return false;
    }

    {
        std::lock_guard lock(s_coord->m_init_tracking_mutex);
        s_coord->m_processes_in_initialization.insert(process_a);
        s_coord->m_processes_in_initialization.insert(process_b);
    }

    const auto release_delayed_workers = [&]() {
        std::ofstream done(
            shared_dir / std::string(k_delayed_done_file),
            std::ios::binary | std::ios::trunc);
        done << "done\n";
    };

    if (!spawn_delayed_worker(binary_path, shared_dir, "a", swarm_id, process_a, coord_id) ||
        !spawn_delayed_worker(binary_path, shared_dir, "b", swarm_id, process_b, coord_id))
    {
        std::fprintf(stderr, "managed_process_publish_test: delayed spawn failed\n");
        release_delayed_workers();
        deactivate_handler();
        return false;
    }

    const auto marked_path = shared_dir / std::string(k_delayed_b_marked_file);
    if (!sintra::test::wait_for_file(marked_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: delayed mark file missing\n");
        release_delayed_workers();
        deactivate_handler();
        return false;
    }

    // The worker closes the name file before publishing this marker, so waiting
    // for the marker prevents observing a created-but-not-yet-written file.
    const auto name_path = shared_dir / std::string(k_delayed_b_name_file);
    std::ifstream name_in(name_path, std::ios::binary);
    std::string expected_name;
    std::getline(name_in >> std::ws, expected_name);
    if (expected_name.empty()) {
        std::fprintf(stderr, "managed_process_publish_test: delayed name file empty\n");
        release_delayed_workers();
        deactivate_handler();
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

    release_delayed_workers();

    const auto exit_path = shared_dir / std::string(k_delayed_exit_file);
    if (!sintra::test::wait_for_file(exit_path, std::chrono::seconds(5))) {
        std::fprintf(stderr, "managed_process_publish_test: delayed worker did not exit\n");
        published = false;
    }

    deactivate_handler();

    std::error_code ec;
    std::filesystem::remove_all(shared_dir, ec);

    return published;
}

} // namespace

int main(int argc, char* argv[])
{
    if (sintra::test::has_argv_flag(argc, argv, k_worker_flag)) {
        return run_worker(argc, argv);
    }
    if (sintra::test::has_argv_flag(argc, argv, k_delayed_worker_flag)) {
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

    bool ok = run_held_reader_publication();
    if (ok) {
        ok = run_local_publication_contract();
    }
    if (!run_drain_timeout_probe()) {
        ok = false;
    }

    if (ok) {
        ok = run_manual_publish_scenario(binary_path, shared_dir);
    }

    if (ok) {
        ok = run_delayed_publication_scenario(binary_path);
    }

    sintra::detail::finalize();

    std::error_code ec;
    std::filesystem::remove_all(shared_dir, ec);

    return ok ? 0 : 1;
}
