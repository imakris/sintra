#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <sintra/detail/utility.h>

namespace {

using namespace std::chrono_literals;

constexpr const char* k_deadlock_child_arg = "--rendezvous_deadlock_child";
constexpr const char* k_deadlock_ready_arg = "--rendezvous_deadlock_ready";

std::string unique_group_name(const char* prefix)
{
    return std::string(prefix) + "_" + std::to_string(static_cast<unsigned long long>(s_mproc_id));
}

bool has_arg(int argc, char* argv[], const char* flag)
{
    if (!flag || !*flag) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg && std::string(arg) == flag) {
            return true;
        }
    }
    return false;
}

std::string get_arg_value(int argc, char* argv[], const char* name)
{
    if (!name || !*name) {
        return {};
    }
    const std::string prefix = std::string(name) + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg.rfind(prefix, 0) == 0) {
            return arg.substr(prefix.size());
        }
        if (arg == name && i + 1 < argc && argv[i + 1]) {
            return std::string(argv[i + 1]);
        }
    }
    return {};
}

std::filesystem::path make_ready_path()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto filename = std::string("sintra_rendezvous_ready_") + std::to_string(now) + ".txt";
    return std::filesystem::temp_directory_path() / filename;
}

bool wait_for_file(const std::filesystem::path& path, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::filesystem::exists(path);
}

bool write_ready_file(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "ready";
    return static_cast<bool>(out);
}

struct Child_process
{
#ifdef _WIN32
    HANDLE handle = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
};

bool wait_for_child_exit(Child_process& child, std::chrono::milliseconds timeout, int& exit_code)
{
#ifdef _WIN32
    const DWORD wait_ms = static_cast<DWORD>(timeout.count());
    DWORD wait_result = WaitForSingleObject(child.handle, wait_ms);
    if (wait_result == WAIT_TIMEOUT) {
        return false;
    }
    if (wait_result != WAIT_OBJECT_0) {
        exit_code = -1;
        return true;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(child.handle, &code)) {
        exit_code = -1;
        return true;
    }
    exit_code = static_cast<int>(code);
    return true;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t result = ::waitpid(child.pid, &status, WNOHANG);
        if (result == child.pid) {
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            } else {
                exit_code = 1;
            }
            return true;
        }
        if (result == 0) {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        if (errno != EINTR) {
            exit_code = -1;
            return true;
        }
    }
    return false;
#endif
}

void terminate_child(Child_process& child)
{
#ifdef _WIN32
    HANDLE handle = child.handle;
    if (!handle && child.pid != 0) {
        handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                             FALSE,
                             child.pid);
    }
    if (handle) {
        TerminateProcess(handle, 99);
        WaitForSingleObject(handle, 2000);
        CloseHandle(handle);
    }
    child.handle = nullptr;
#else
    if (child.pid > 0) {
        ::kill(child.pid, SIGKILL);
        ::waitpid(child.pid, nullptr, 0);
    }
#endif
}

// Child process entry point for deadlock test.
// Communication is NOT paused here - we want the barrier to actually block
// waiting for the non-existent peer, rather than treating RPC failures as satisfied.
int run_deadlock_child(const std::string& ready_path)
{
    // Do NOT pause communication here. The barrier should block forever waiting
    // for the missing peer. If we paused, RPC failures would be treated as
    // "satisfied" and the barrier would return early.

    bool ok = true;

    const auto group_name = unique_group_name("rendezvous_deadlock_group");
    const auto barrier_name = std::string("rendezvous_deadlock_barrier");

    // Use a process index that is definitely not local.
    // The local process index is typically 2 (first user process after coordinator).
    // We use index 3 to ensure we're referencing a non-existent remote process.
    const uint32_t local_index = sintra::get_process_index(s_mproc_id);
    uint32_t remote_index = local_index + 1;
    if (remote_index > sintra::max_process_index) {
        remote_index = (local_index > 2) ? (local_index - 1) : local_index;
    }
    if (remote_index == local_index || remote_index == 1) {
        std::fprintf(stderr, "Could not determine a valid remote process index.\n");
        sintra::finalize();
        return 1;
    }

    std::unordered_set<sintra::instance_id_type> members;
    members.insert(s_mproc_id);
    members.insert(sintra::make_process_instance_id(remote_index));
    sintra::Process_group group;
    group.set(members);

    if (!group.assign_name(group_name)) {
        std::fprintf(stderr, "Failed to assign group name for deadlock test.\n");
        ok = false;
    } else {
        if (!ready_path.empty()) {
            if (!write_ready_file(ready_path)) {
                std::fprintf(stderr, "Failed to write ready file for deadlock test.\n");
                ok = false;
            }
        }

        if (!ok) {
            sintra::finalize();
            return 1;
        }

        // This should block forever because the remote process doesn't exist.
        const bool barrier_result =
            sintra::barrier<sintra::rendezvous_t>(barrier_name, group_name);

        // If we reach here, the barrier didn't deadlock as expected.
        std::fprintf(stderr,
                     "Expected rendezvous barrier to deadlock; got result=%s\n",
                     barrier_result ? "true" : "false");
        ok = false;
    }

    sintra::finalize();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    if (has_arg(argc, argv, k_deadlock_child_arg)) {
        sintra::init(argc, argv);
        const std::string ready_path = get_arg_value(argc, argv, k_deadlock_ready_arg);
        return run_deadlock_child(ready_path);
    }

    sintra::init(argc, argv);

    const auto previous_state = s_mproc->m_communication_state;
    s_mproc->m_communication_state = sintra::Managed_process::COMMUNICATION_PAUSED;

    bool ok = true;

    // Case 1: rpc_cancelled path in rendezvous_barrier.
    // Keep the group alive until finalize() so any late request reader dispatch
    // still finds a valid local receiver after cancellation.
    sintra::Process_group cancel_group;
    {
        const auto group_name = unique_group_name("rendezvous_cancel_group");
        const auto barrier_name = std::string("rendezvous_cancel_barrier");

        std::unordered_set<sintra::instance_id_type> members;
        members.insert(s_mproc_id);
        members.insert(sintra::make_process_instance_id(2));
        cancel_group.set(members);

        if (!cancel_group.assign_name(group_name)) {
            std::fprintf(stderr, "Failed to assign group name for cancel test.\n");
            ok = false;
        } else {
            std::atomic<bool> barrier_result{false};
            std::atomic<bool> barrier_done{false};
            std::thread waiter([&] {
                barrier_result = sintra::barrier<sintra::rendezvous_t>(barrier_name, group_name);
                barrier_done = true;
            });

            bool cancelled = false;
            const auto cancel_deadline = std::chrono::steady_clock::now() + 5s;
            while (!barrier_done.load() &&
                   std::chrono::steady_clock::now() < cancel_deadline)
            {
                if (s_mproc->unblock_rpc(sintra::process_of(s_coord_id)) > 0) {
                    cancelled = true;
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }

            waiter.join();

            if (!cancelled) {
                std::fprintf(stderr,
                             "Did not observe cancellable outstanding RPC (cancel test).\n");
                ok = false;
            }

            if (cancelled && !barrier_result.load()) {
                std::fprintf(stderr, "Expected rendezvous barrier to return true after cancellation.\n");
                ok = false;
            }
        }
    }

    // Case 2: runtime_error path in rendezvous_barrier (target shutting down).
    {
        sintra::Process_group group;
        const auto group_name = unique_group_name("rendezvous_shutdown_group");
        const auto barrier_name = std::string("rendezvous_shutdown_barrier");

        std::unordered_set<sintra::instance_id_type> members;
        members.insert(s_mproc_id);
        group.set(members);

        if (!group.assign_name(group_name)) {
            std::fprintf(stderr, "Failed to assign group name for shutdown test.\n");
            ok = false;
        } else {
            const auto group_iid = group.instance_id();
            group.destroy();
            s_mproc->m_instance_id_of_assigned_name[group_name] = group_iid;
            s_mproc->m_local_pointer_of_instance_id[group_iid] = &group;
            sintra::Transceiver::get_instance_to_object_map<sintra::Process_group::barrier_mftc>()[group_iid] = &group;

            const bool barrier_result =
                sintra::barrier<sintra::rendezvous_t>(barrier_name, group_name);

            if (!barrier_result) {
                std::fprintf(stderr, "Expected rendezvous barrier to return true for shutdown target.\n");
                ok = false;
            }

            {
                auto scoped = s_mproc->m_instance_id_of_assigned_name.scoped();
                scoped.get().erase(group_name);
            }
            {
                auto scoped = s_mproc->m_local_pointer_of_instance_id.scoped();
                scoped.get().erase(group_iid);
            }
            {
                auto scoped =
                    sintra::Transceiver::get_instance_to_object_map<sintra::Process_group::barrier_mftc>().scoped();
                scoped.get().erase(group_iid);
            }
        }
    }

    // Case 3: Deadlock path - barrier should hang when peer doesn't exist.
    // This runs in a child process with communication NOT paused, so the barrier
    // actually blocks instead of treating RPC failures as satisfied.
    {
        const auto ready_path = make_ready_path();
        const std::string ready_arg = std::string(k_deadlock_ready_arg) + "=" + ready_path.string();
        std::vector<const char*> args;
        args.reserve(4);
        args.push_back(argv[0]);
        args.push_back(k_deadlock_child_arg);
        args.push_back(ready_arg.c_str());
        args.push_back(nullptr);

        int child_pid = -1;
        sintra::Spawn_detached_options options;
        options.prog = argv[0];
        options.argv = args.data();
        options.child_pid_out = &child_pid;

        if (!sintra::spawn_detached(options) || child_pid <= 0) {
            std::fprintf(stderr, "Failed to spawn deadlock child process.\n");
            ok = false;
        } else {
            if (!wait_for_file(ready_path, 2s)) {
                std::fprintf(stderr, "Deadlock child did not signal readiness.\n");
                ok = false;
            }

            Child_process child;
#ifdef _WIN32
            child.pid = static_cast<DWORD>(child_pid);
            child.handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE,
                                       child.pid);
            if (!child.handle) {
                std::fprintf(stderr, "Failed to open deadlock child process handle.\n");
                ok = false;
            }
#else
            child.pid = static_cast<pid_t>(child_pid);
#endif

            if (ok) {
                int exit_code = 0;
                const bool exited = wait_for_child_exit(child, 750ms, exit_code);
                if (exited) {
                    std::fprintf(stderr,
                                 "Deadlock child exited early (code %d); expected hang.\n",
                                 exit_code);
                    ok = false;
                }
            }

            terminate_child(child);
            std::filesystem::remove(ready_path);
        }
    }

    s_mproc->m_communication_state = previous_state;
    sintra::finalize();
    return ok ? 0 : 1;
}
