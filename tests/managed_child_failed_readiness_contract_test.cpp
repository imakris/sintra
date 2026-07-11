//
// Managed-child failed-readiness baseline contract evidence (R2/R3).
//

#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/runtime.h>

#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view k_child_flag = "--managed_child_failed_readiness_child";
constexpr std::string_view k_nonce_flag = "--managed_child_failed_readiness_nonce";
constexpr std::string_view k_child_ledger_file = "child_ledger.complete";
constexpr std::string_view k_child_release_file = "child_release.complete";
constexpr std::string_view k_child_release_seen_file = "child_release_seen.complete";
constexpr std::string_view k_child_finalized_file = "child_finalized.complete";
constexpr auto k_requested_wait_timeout = std::chrono::milliseconds(350);
constexpr auto k_watchdog_timeout = std::chrono::seconds(10);
constexpr sintra::instance_id_type k_child_process_iid = sintra::compose_instance(29u, 1ull);

struct Child_ledger
{
    std::string                nonce;
    sintra::instance_id_type   process_iid = sintra::invalid_instance_id;
    uint32_t                   occurrence = std::numeric_limits<uint32_t>::max();
    int                        pid = -1;
    bool                       start_stamp_available = false;
    uint64_t                   start_stamp = 0;
    std::string                managed_name;
};

struct Spawn_gate
{
    std::mutex               mutex;
    std::condition_variable  cv;
    bool                     entered = false;
    bool                     released = false;
};

struct Spawn_call
{
    std::mutex               mutex;
    std::condition_variable  cv;
    bool                     done = false;
    sintra::Managed_child_custody custody;
    bool                     threw = false;
};

Spawn_gate* s_spawn_gate = nullptr;

fs::path child_ledger_path(const fs::path& dir)
{
    return dir / std::string(k_child_ledger_file);
}

fs::path child_release_path(const fs::path& dir)
{
    return dir / std::string(k_child_release_file);
}

fs::path child_release_seen_path(const fs::path& dir)
{
    return dir / std::string(k_child_release_seen_file);
}

fs::path child_finalized_path(const fs::path& dir)
{
    return dir / std::string(k_child_finalized_file);
}

bool write_complete_file(const fs::path& path, const std::string& contents)
{
    const auto temporary_path = path.string() + ".tmp";

#ifdef _WIN32
    const auto temporary_wide = fs::path(temporary_path).wstring();
    const auto final_wide = path.wstring();
    HANDLE file = CreateFileW(
        temporary_wide.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    size_t offset = 0;
    bool write_ok = true;
    while (offset < contents.size()) {
        const auto remaining = contents.size() - offset;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(
            remaining,
            static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, chunk, &written, nullptr) || written == 0) {
            write_ok = false;
            break;
        }
        offset += written;
    }

    const bool flush_ok = write_ok && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flush_ok ||
        !MoveFileExW(
            temporary_wide.c_str(),
            final_wide.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary_wide.c_str());
        return false;
    }
    return true;
#else
    const int file = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) {
        return false;
    }

    size_t offset = 0;
    bool write_ok = true;
    while (offset < contents.size()) {
        const ssize_t written = ::write(
            file,
            contents.data() + offset,
            contents.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            write_ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }

    const bool flush_ok = write_ok && ::fsync(file) == 0;
    const bool close_ok = ::close(file) == 0;
    if (!flush_ok || !close_ok || ::rename(temporary_path.c_str(), path.c_str()) != 0) {
        ::unlink(temporary_path.c_str());
        return false;
    }

    const auto parent = path.parent_path();
    const int directory = ::open(parent.c_str(), O_RDONLY);
    if (directory >= 0) {
        const bool directory_flush_ok =
            ::fsync(directory) == 0 || errno == EINVAL || errno == EBADF;
        ::close(directory);
        if (!directory_flush_ok) {
            return false;
        }
    }
    return true;
#endif
}

void spawn_stage_callback(const char* stage)
{
    if (!stage ||
        std::strcmp(
            stage,
            sintra::detail::test_hooks::k_stage_spawn_success_before_readiness_wait) != 0)
    {
        return;
    }

    Spawn_gate* gate = s_spawn_gate;
    if (!gate) {
        return;
    }

    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->entered = true;
    gate->cv.notify_all();
    gate->cv.wait(lock, [&]() {
        return gate->released;
    });
}

bool wait_for_spawn_stage(Spawn_gate& gate, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.cv.wait_for(lock, timeout, [&]() {
        return gate.entered;
    });
}

void release_spawn_stage(Spawn_gate& gate)
{
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.released = true;
    gate.cv.notify_all();
}

bool wait_for_spawn_call(Spawn_call& call, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(call.mutex);
    return call.cv.wait_for(lock, timeout, [&]() {
        return call.done;
    });
}

std::optional<Child_ledger> read_child_ledger(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    Child_ledger ledger;
    bool complete = false;
    std::string line;
    try {
        while (std::getline(in, line)) {
            const auto separator = line.find('=');
            if (separator == std::string::npos) {
                return std::nullopt;
            }
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (key == "nonce") {
                ledger.nonce = value;
            }
            else
            if (key == "piid") {
                ledger.process_iid = static_cast<sintra::instance_id_type>(std::stoull(value));
            }
            else
            if (key == "occurrence") {
                ledger.occurrence = static_cast<uint32_t>(std::stoul(value));
            }
            else
            if (key == "pid") {
                ledger.pid = std::stoi(value);
            }
            else
            if (key == "start_stamp_available") {
                ledger.start_stamp_available = value == "1";
            }
            else
            if (key == "start_stamp") {
                ledger.start_stamp = std::stoull(value);
            }
            else
            if (key == "managed_name") {
                ledger.managed_name = value;
            }
            else
            if (key == "complete") {
                complete = value == "1";
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }

    if (!complete || ledger.nonce.empty() || ledger.pid <= 0 || ledger.managed_name.empty()) {
        return std::nullopt;
    }
    return ledger;
}

std::optional<sintra::instance_id_type> resolve_with_public_rpc(const std::string& name)
{
    if (sintra::s_coord_id == sintra::invalid_instance_id) {
        return std::nullopt;
    }

    try {
        return sintra::Coordinator::rpc_resolve_instance(sintra::s_coord_id, name);
    }
    catch (...) {
        return std::nullopt;
    }
}

bool wait_for_public_resolution(
    const std::string&             name,
    sintra::instance_id_type       expected,
    std::chrono::milliseconds      timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto resolved = resolve_with_public_rpc(name);
        if (resolved && *resolved == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto resolved = resolve_with_public_rpc(name);
    return resolved && *resolved == expected;
}

bool wait_for_public_absence(const std::string& name, std::chrono::milliseconds timeout)
{
    return wait_for_public_resolution(name, sintra::invalid_instance_id, timeout);
}

#ifndef _WIN32
struct Posix_reap_observation
{
    std::atomic<pid_t>     expected_pid{-1};
    std::atomic<uint32_t>  count{0};
    std::atomic<int>       status{0};
};

Posix_reap_observation s_posix_reap;

void posix_child_reaped(pid_t pid, int status) noexcept
{
    if (s_posix_reap.expected_pid.load(std::memory_order_acquire) != pid) {
        return;
    }

    s_posix_reap.status.store(status, std::memory_order_relaxed);
    s_posix_reap.count.fetch_add(1, std::memory_order_release);
    s_posix_reap.count.notify_all();
}

bool wait_for_posix_reap(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (s_posix_reap.count.load(std::memory_order_acquire) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return s_posix_reap.count.load(std::memory_order_acquire) != 0;
}

bool exact_posix_child_is_live(int pid, uint64_t start_stamp)
{
    if (pid <= 0 || !sintra::is_process_alive(static_cast<uint32_t>(pid))) {
        return false;
    }
    const auto observed_stamp = sintra::query_process_start_stamp(static_cast<uint32_t>(pid));
    return observed_stamp && *observed_stamp == start_stamp;
}

bool signal_exact_posix_child(int pid, uint64_t start_stamp, int signal)
{
    if (!exact_posix_child_is_live(pid, start_stamp)) {
        return false;
    }
    return ::kill(static_cast<pid_t>(pid), signal) == 0;
}
#endif

int run_child(int argc, char* argv[], const fs::path& shared_dir)
{
    const std::string nonce = sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    if (nonce.empty()) {
        std::fprintf(stderr, "managed_child_failed_readiness: child nonce missing\n");
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_failed_readiness: child init failed: %s\n", error.what());
        return 2;
    }

    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();
    const std::string managed_name = "sintra_process_" + std::to_string(pid);

    std::ostringstream marker;
    marker << "nonce=" << nonce << '\n'
           << "piid=" << static_cast<unsigned long long>(sintra::s_mproc_id) << '\n'
           << "occurrence=" << sintra::s_recovery_occurrence << '\n'
           << "pid=" << pid << '\n'
           << "start_stamp_available=" << (start_stamp.has_value() ? 1 : 0) << '\n'
           << "start_stamp=" << start_stamp.value_or(0) << '\n'
           << "managed_name=" << managed_name << '\n'
           << "complete=1\n";

    if (!write_complete_file(child_ledger_path(shared_dir), marker.str())) {
        std::fprintf(stderr, "managed_child_failed_readiness: child ledger write failed\n");
        sintra::detail::finalize();
        return 2;
    }

    const bool released = sintra::test::wait_for_file(
        child_release_path(shared_dir),
        std::chrono::seconds(30),
        std::chrono::milliseconds(10));
    if (!released ||
        !write_complete_file(child_release_seen_path(shared_dir), "release_seen=1\ncomplete=1\n"))
    {
        std::fprintf(stderr, "managed_child_failed_readiness: child release latch failed\n");
        sintra::detail::finalize();
        return 2;
    }

    bool finalized = false;
    try {
        finalized = sintra::detail::finalize();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_failed_readiness: child finalize failed: %s\n", error.what());
        return 2;
    }
    catch (...) {
        std::fprintf(stderr, "managed_child_failed_readiness: child finalize failed\n");
        return 2;
    }

    if (!finalized ||
        !write_complete_file(child_finalized_path(shared_dir), "finalized=1\ncomplete=1\n"))
    {
        std::fprintf(stderr, "managed_child_failed_readiness: child did not finalize cleanly\n");
        return 2;
    }
    return 0;
}

int run_root(int argc, char* argv[], sintra::test::Shared_directory& shared)
{
    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    if (binary_path.empty()) {
        std::fprintf(stderr, "managed_child_failed_readiness: binary path missing\n");
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_failed_readiness: root init failed: %s\n", error.what());
        return 2;
    }

    const auto nonce_value = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string nonce = std::to_string(nonce_value) + "_" +
        std::to_string(sintra::test::get_pid());
    const std::string requested_target = "managed_child_requested_" + nonce;

    std::atomic<bool> requested_target_seen{false};
    auto publication_handler = [
            &requested_target,
            &requested_target_seen
        ](const sintra::Coordinator::instance_published& message)
    {
        const std::string name = static_cast<std::string>(message.assigned_name);
        if (name == requested_target) {
            requested_target_seen.store(true, std::memory_order_release);
        }
    };
    sintra::activate_slot(
        publication_handler,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    Spawn_gate gate;
    Spawn_call call;
    s_spawn_gate = &gate;
    sintra::detail::test_hooks::s_runtime_stage.store(
        &spawn_stage_callback,
        std::memory_order_release);
#ifndef _WIN32
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &posix_child_reaped,
        std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_child_flag),
        std::string(k_nonce_flag),
        nonce,
    };
    options.process_instance_id = k_child_process_iid;
    options.wait_for_instance_name = requested_target;
    options.wait_timeout = k_requested_wait_timeout;
    options.lifetime.enable_lifeline = false;

    std::thread spawn_thread([&]() {
        try {
            call.custody = sintra::spawn_swarm_process(options);
        }
        catch (...) {
            call.threw = true;
        }
        {
            std::lock_guard<std::mutex> lock(call.mutex);
            call.done = true;
        }
        call.cv.notify_all();
    });

    bool setup_valid = true;
    const bool spawn_success_stage = wait_for_spawn_stage(gate, k_watchdog_timeout);
    if (!spawn_success_stage) {
        std::fprintf(stderr, "managed_child_failed_readiness: spawn-success stage not reached\n");
        setup_valid = false;
    }

    const bool ledger_file_seen = sintra::test::wait_for_file(
        child_ledger_path(shared.path()),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto ledger = ledger_file_seen
        ? read_child_ledger(child_ledger_path(shared.path()))
        : std::nullopt;
    if (!ledger) {
        std::fprintf(stderr, "managed_child_failed_readiness: complete child ledger missing or invalid\n");
        setup_valid = false;
    }

    bool ledger_identity_valid = false;
    bool start_stamp_verified = false;
    bool native_identity_verified = false;
    bool native_alive_before_release = false;
    bool managed_name_seen = false;
    bool requested_target_absent_before = false;

#ifdef _WIN32
    HANDLE child_process = nullptr;
#endif

    if (ledger) {
        const std::string expected_managed_name = "sintra_process_" + std::to_string(ledger->pid);
        ledger_identity_valid =
            ledger->nonce == nonce &&
            ledger->process_iid == k_child_process_iid &&
            ledger->occurrence == 0 &&
            ledger->managed_name == expected_managed_name;
        if (!ledger_identity_valid) {
            std::fprintf(stderr, "managed_child_failed_readiness: child ledger identity mismatch\n");
            setup_valid = false;
        }

#ifdef _WIN32
        child_process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE,
            static_cast<DWORD>(ledger->pid));
        const auto observed_start_stamp = sintra::query_process_start_stamp(
            static_cast<uint32_t>(ledger->pid));
        start_stamp_verified =
            ledger->start_stamp_available &&
            observed_start_stamp.has_value() &&
            *observed_start_stamp == ledger->start_stamp;
        native_identity_verified = start_stamp_verified;
        native_alive_before_release =
            child_process &&
            WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT &&
            native_identity_verified;
#else
        if (ledger_identity_valid) {
            s_posix_reap.expected_pid.store(
                static_cast<pid_t>(ledger->pid),
                std::memory_order_release);
        }
        const auto observed_start_stamp = sintra::query_process_start_stamp(
            static_cast<uint32_t>(ledger->pid));
        start_stamp_verified =
            ledger->start_stamp_available &&
            observed_start_stamp.has_value() &&
            *observed_start_stamp == ledger->start_stamp;
        native_identity_verified =
            start_stamp_verified &&
            exact_posix_child_is_live(ledger->pid, ledger->start_stamp);
        native_alive_before_release = native_identity_verified;
#endif

        managed_name_seen = wait_for_public_resolution(
            ledger->managed_name,
            k_child_process_iid,
            std::chrono::seconds(3));
        const auto requested_before = resolve_with_public_rpc(requested_target);
        requested_target_absent_before =
            requested_before && *requested_before == sintra::invalid_instance_id;
        if (!native_alive_before_release ||
            !managed_name_seen ||
            !requested_target_absent_before ||
            requested_target_seen.load(std::memory_order_acquire))
        {
            std::fprintf(stderr, "managed_child_failed_readiness: pre-release causal facts incomplete\n");
            setup_valid = false;
        }
    }

    release_spawn_stage(gate);

    bool spawn_call_completed = wait_for_spawn_call(call, k_watchdog_timeout);
    bool child_release_written = false;
    if (!spawn_call_completed) {
        std::fprintf(stderr, "managed_child_failed_readiness: spawn call did not complete after hook release\n");
        setup_valid = false;
    }

    bool spawn_result_invalid = false;
    if (spawn_call_completed) {
        std::lock_guard<std::mutex> lock(call.mutex);
        const auto observation = sintra::observe_managed_child(call.custody);
        spawn_result_invalid = call.threw || !observation.accepted ||
            observation.created_occurrences != 1 || observation.readiness_reached ||
            !observation.release_requested;
    }
    if (!setup_valid || !spawn_call_completed || spawn_result_invalid) {
        child_release_written = write_complete_file(
            child_release_path(shared.path()),
            "release=1\ncomplete=1\n");
    }

    spawn_thread.join();
    spawn_call_completed = call.done;
    sintra::detail::test_hooks::s_runtime_stage.store(nullptr, std::memory_order_release);
    s_spawn_gate = nullptr;

    const auto launch_observation = sintra::observe_managed_child(call.custody);
    const bool custody_retained = spawn_call_completed && !call.threw &&
        launch_observation.accepted && launch_observation.created_occurrences == 1 &&
        !launch_observation.readiness_reached && launch_observation.release_requested &&
        !launch_observation.release_complete;
    bool managed_name_absent_after = false;
    bool requested_target_absent_after = false;
    bool native_alive_after = false;

    if (ledger) {
        managed_name_absent_after = wait_for_public_absence(
            ledger->managed_name,
            std::chrono::seconds(3));
        const auto requested_after = resolve_with_public_rpc(requested_target);
        requested_target_absent_after =
            requested_after && *requested_after == sintra::invalid_instance_id;
#ifdef _WIN32
        native_alive_after = child_process &&
            WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT &&
            sintra::query_process_start_stamp(static_cast<uint32_t>(ledger->pid)) ==
                std::optional<uint64_t>(ledger->start_stamp);
#else
        native_alive_after = exact_posix_child_is_live(ledger->pid, ledger->start_stamp);
#endif
    }

    const bool requested_target_never_seen =
        requested_target_absent_before &&
        requested_target_absent_after &&
        !requested_target_seen.load(std::memory_order_acquire);

    if (!child_release_written) {
        child_release_written = write_complete_file(
            child_release_path(shared.path()),
            "release=1\ncomplete=1\n");
    }

    bool native_exit_confirmed = false;
    bool native_normal_exit = false;
    bool forced_cleanup = false;
    bool survivor_absent = false;
#ifndef _WIN32
    uint32_t reap_count = 0;
    int reap_status = 0;
    bool child_reap_hook_cleared = false;
#else
    const bool child_reap_hook_cleared = true;
#endif
    if (ledger && child_release_written) {
#ifdef _WIN32
        if (child_process && WaitForSingleObject(child_process, 10000) == WAIT_OBJECT_0) {
            DWORD exit_code = STILL_ACTIVE;
            native_exit_confirmed = GetExitCodeProcess(child_process, &exit_code) != 0;
            native_normal_exit = native_exit_confirmed && exit_code == 0;
        }
#else
        wait_for_posix_reap(k_watchdog_timeout);
#endif
        survivor_absent = native_exit_confirmed;
    }

    const bool release_seen = sintra::test::wait_for_file(
        child_release_seen_path(shared.path()),
        std::chrono::seconds(1),
        std::chrono::milliseconds(10));
    const bool child_finalized = sintra::test::wait_for_file(
        child_finalized_path(shared.path()),
        std::chrono::seconds(1),
        std::chrono::milliseconds(10));
    const auto released_observation = sintra::wait_managed_child(
        call.custody,
        std::chrono::steady_clock::now() + k_watchdog_timeout);

#ifdef _WIN32
    if (ledger && !native_exit_confirmed) {
        if (child_process &&
            WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT &&
            sintra::query_process_start_stamp(static_cast<uint32_t>(ledger->pid)) ==
                std::optional<uint64_t>(ledger->start_stamp))
        {
            forced_cleanup = TerminateProcess(child_process, 2) != 0;
            survivor_absent = WaitForSingleObject(child_process, 2000) == WAIT_OBJECT_0;
        }
    }
    if (child_process) {
        CloseHandle(child_process);
    }
#else
    if (ledger && s_posix_reap.count.load(std::memory_order_acquire) == 0) {
        if (signal_exact_posix_child(ledger->pid, ledger->start_stamp, SIGTERM)) {
            forced_cleanup = true;
            wait_for_posix_reap(std::chrono::seconds(2));
        }
        if (s_posix_reap.count.load(std::memory_order_acquire) == 0 &&
            signal_exact_posix_child(ledger->pid, ledger->start_stamp, SIGKILL))
        {
            forced_cleanup = true;
            wait_for_posix_reap(std::chrono::seconds(2));
        }
    }
#endif

    sintra::deactivate_all_slots();
    bool root_finalized = false;
    try {
        root_finalized = sintra::detail::finalize();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_failed_readiness: root finalize failed: %s\n", error.what());
    }
    catch (...) {
        std::fprintf(stderr, "managed_child_failed_readiness: root finalize failed\n");
    }

#ifndef _WIN32
    if (root_finalized) {
        wait_for_posix_reap(std::chrono::seconds(1));
        reap_count = s_posix_reap.count.load(std::memory_order_acquire);
        reap_status = s_posix_reap.status.load(std::memory_order_relaxed);
        native_exit_confirmed = reap_count == 1;
        native_normal_exit =
            native_exit_confirmed &&
            WIFEXITED(reap_status) &&
            WEXITSTATUS(reap_status) == 0;
        survivor_absent =
            native_exit_confirmed &&
            ledger &&
            !exact_posix_child_is_live(ledger->pid, ledger->start_stamp);

        sintra::detail::test_hooks::s_child_reaped.store(nullptr, std::memory_order_release);
        s_posix_reap.expected_pid.store(-1, std::memory_order_release);
        child_reap_hook_cleared = true;
    }
#endif

    const bool baseline_valid =
        setup_valid &&
        spawn_success_stage &&
        ledger_identity_valid &&
        native_identity_verified &&
        native_alive_before_release &&
        managed_name_seen &&
        requested_target_never_seen &&
        custody_retained &&
        managed_name_absent_after &&
        native_alive_after &&
        child_release_written &&
        release_seen &&
        child_finalized &&
        released_observation.release_complete &&
        native_exit_confirmed &&
        native_normal_exit &&
        survivor_absent &&
        !forced_cleanup &&
        child_reap_hook_cleared &&
        root_finalized;

    const unsigned long long output_piid = ledger
        ? static_cast<unsigned long long>(ledger->process_iid)
        : 0ull;
    const unsigned output_occurrence = ledger ? ledger->occurrence : std::numeric_limits<unsigned>::max();
    const int output_pid = ledger ? ledger->pid : -1;
    const std::string output_start_stamp = ledger && ledger->start_stamp_available
        ? std::to_string(ledger->start_stamp)
        : "unavailable";
#ifdef _WIN32
    const std::string reap_record = "reap_count=not_applicable reap_status=not_applicable normal_status=" +
        std::to_string(native_normal_exit ? 1 : 0);
#else
    const std::string reap_record = "reap_count=" + std::to_string(reap_count) +
        " reap_status=" + std::to_string(reap_status) +
        " normal_status=" + std::to_string(native_normal_exit ? 1 : 0);
#endif

    if (baseline_valid) {
        std::printf(
            "R2_GREEN_VALID R3_GREEN_VALID nonce=%s piid=%llu occurrence=%u pid=%d "
            "start_stamp=%s start_stamp_verified=%d native_identity_verified=1 "
            "spawn_success_stage=1 "
            "managed_name_seen=1 requested_target_seen=0 custody_accepted=1 "
            "readiness=0 release_requested=1 release_complete=1 "
            "managed_name_after=absent native_alive_after=1 native_exit_confirmed=1 "
            "survivor_absent=1 %s\n",
            nonce.c_str(),
            output_piid,
            output_occurrence,
            output_pid,
            output_start_stamp.c_str(),
            start_stamp_verified ? 1 : 0,
            reap_record.c_str());
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(
        stderr,
        "R2_R3_INVALID nonce=%s piid=%llu occurrence=%u pid=%d start_stamp=%s "
        "start_stamp_verified=%d native_identity_verified=%d spawn_success_stage=%d "
        "managed_name_seen=%d requested_target_seen=%d custody_retained=%d "
        "managed_name_after=%s native_alive_after=%d native_exit_confirmed=%d "
        "survivor_absent=%d forced_cleanup=%d root_finalized=%d "
        "child_reap_hook_cleared=%d %s\n",
        nonce.c_str(),
        output_piid,
        output_occurrence,
        output_pid,
        output_start_stamp.c_str(),
        start_stamp_verified ? 1 : 0,
        native_identity_verified ? 1 : 0,
        spawn_success_stage ? 1 : 0,
        managed_name_seen ? 1 : 0,
        requested_target_seen.load(std::memory_order_acquire) ? 1 : 0,
        custody_retained ? 1 : 0,
        managed_name_absent_after ? "absent" : "present_or_unknown",
        native_alive_after ? 1 : 0,
        native_exit_confirmed ? 1 : 0,
        survivor_absent ? 1 : 0,
        forced_cleanup ? 1 : 0,
        root_finalized ? 1 : 0,
        child_reap_hook_cleared ? 1 : 0,
        reap_record.c_str());
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_MANAGED_CHILD_FAILED_READINESS_DIR",
        "managed_child_failed_readiness_contract");

    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared);
}
