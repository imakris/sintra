//
// Sintra Mid-Flight Join Test
//
// Validates that the coordinator can pull a new process into the swarm while
// running, wire up its rings, add it to the default groups, and let it
// participate in barriers and broadcasts.
//

#include <sintra/sintra.h>

#include "test_environment.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* k_shared_dir_env = "SINTRA_JOIN_SWARM_DIR";

struct Hello {
    int sender;
    int seq;
};
struct Ping { int token; };

std::filesystem::path shared_dir()
{
    const char* value = std::getenv(k_shared_dir_env);
    if (!value || !*value) {
        throw std::runtime_error("SINTRA_JOIN_SWARM_DIR is not set");
    }
    return std::filesystem::path(value);
}

void append_line(const std::filesystem::path& file, const std::string& line)
{
    // Use a narrow string path and treat logging as best-effort only.
    // Any failure to append should not crash the process – tests will
    // detect missing lines via read_lines().
    try {
        const auto path_str = file.string();
        std::ofstream out(path_str, std::ios::binary | std::ios::app);
        if (!out) {
            return;
        }
        out << line << '\n';
    } catch (...) {
        // Swallow I/O errors in test logging; functional correctness is
        // validated by the presence/absence of lines, not by logging itself.
    }
}

std::vector<std::string> read_lines(const std::filesystem::path& file)
{
    std::vector<std::string> lines;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return lines;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

struct Ping_receiver : sintra::Derived_transceiver<Ping_receiver>
{
    int ping(int token)
    {
        // Simple echo to prove direct RPC delivery.
        return token + 1;
    }

    SINTRA_RPC(ping);
};

long long monotonic_millis()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int process_id()
{
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

void trace_event(const std::filesystem::path& trace_path, const char* stage, const std::string& detail)
{
    std::ostringstream oss;
    oss << "[" << monotonic_millis() << " ms]"
        << " pid=" << process_id()
        << " [" << stage << "] " << detail;
    const auto line = oss.str();
    append_line(trace_path, line);
    std::cerr << "[join_swarm_midflight] " << line << std::endl;
}

int worker()
{
    const auto dir = shared_dir();
    const auto log_path = dir / "hello.log";
    const auto trace_path = dir / "trace.log";
    const auto initiator_marker = dir / "initiator_claimed";
    const auto process_iid = sintra::process_of(s_mproc_id);
    sintra::instance_id_type joined_process = sintra::invalid_instance_id;

    auto hello_slot = [log_path](Hello h) {
        std::ostringstream oss;
        oss << "recv=" << sintra::process_of(s_mproc_id)
            << " sender=" << h.sender
            << " seq=" << h.seq;
        append_line(log_path, oss.str());
    };
    sintra::activate_slot(hello_slot);

    trace_event(trace_path, "worker_start",
        "sintra_process=" + std::to_string(process_iid) +
        " dir=" + dir.string());

    // Publish a direct-call target so other processes can reach us by name.
    Ping_receiver ping_receiver;
    const auto ping_name = std::string("ping_") + std::to_string(process_iid);
    const bool named = ping_receiver.assign_name(ping_name);
    trace_event(trace_path, "ping_setup",
        "name=" + ping_name +
        " ping_iid=" + std::to_string(ping_receiver.instance_id()) +
        " named=" + std::string(named ? "1" : "0"));

    const bool initiator = std::filesystem::create_directory(initiator_marker);
    trace_event(trace_path, "role",
        std::string("sintra_process=") + std::to_string(process_iid) +
        (initiator ? " initiator" : " follower"));

    if (initiator) {
        trace_event(trace_path, "join_swarm.begin", "requesting additional process");
        joined_process = sintra::join_swarm(1);
        trace_event(trace_path, "join_swarm.end", "joined=" + std::to_string(joined_process));
        if (joined_process == sintra::invalid_instance_id) {
            return 1;
        }
    }

    // Ensure both the existing worker and the newly joined worker rendezvous.
    trace_event(trace_path, "barrier.enter", "post-join-sync");
    sintra::barrier("post-join-sync", "_sintra_external_processes");
    trace_event(trace_path, "barrier.exit", "post-join-sync");

    if (initiator && joined_process != sintra::invalid_instance_id) {
        // Probe the newly joined process via direct RPC (not world broadcast) after the barrier.
        const auto target_name = std::string("ping_") + std::to_string(joined_process);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int attempts = 0;
        bool success = false;
        std::string last_error;
        while (std::chrono::steady_clock::now() < deadline && !success) {
            ++attempts;
            try {
                const int result = Ping_receiver::rpc_ping(target_name, 42);
                trace_event(trace_path, "direct_ping.ok",
                    "target=" + target_name + " result=" + std::to_string(result) +
                    " attempts=" + std::to_string(attempts));
                success = true;
            } catch (const std::exception& e) {
                last_error = e.what();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!success) {
            trace_event(trace_path, "direct_ping.error",
                "target=" + target_name + " attempts=" + std::to_string(attempts) +
                " error=" + last_error);
        }
    }

    // Both processes broadcast after the barrier to help diagnose group membership/broadcast coverage.
    if (initiator) {
        trace_event(trace_path, "broadcast", "coordinator broadcasting Hello");
        sintra::world() << Hello{static_cast<int>(process_iid), 1};
    } else {
        trace_event(trace_path, "broadcast", "follower broadcasting Hello");
        sintra::world() << Hello{static_cast<int>(process_iid), 2};
    }

    // Wait until both workers have handled the broadcast.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t seen = 0;
    size_t last_reported = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        seen = read_lines(log_path).size();
        if (seen != last_reported) {
            trace_event(trace_path, "progress",
                "sintra_process=" + std::to_string(sintra::process_of(s_mproc_id)) +
                " seen=" + std::to_string(seen));
            last_reported = seen;
        }
        if (seen >= 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (seen < 2) {
        const auto contents = read_lines(log_path);
        std::ostringstream oss;
        oss << "worker exit seen=" << seen << " log=[";
        for (size_t i = 0; i < contents.size(); ++i) {
            if (i) {
                oss << ", ";
            }
            oss << contents[i];
        }
        oss << "]";
        trace_event(trace_path, "worker_exit", oss.str());
    } else {
        trace_event(trace_path, "worker_exit", "seen=" + std::to_string(seen));
    }

    return seen >= 2 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const char* existing_dir = std::getenv(k_shared_dir_env);
    const bool own_dir = !(existing_dir && *existing_dir);
    const auto dir = own_dir
        ? sintra::test::unique_scratch_directory("join_swarm_midflight")
        : std::filesystem::path(existing_dir);
    std::filesystem::create_directories(dir);
    const auto log_path = dir / "hello.log";
    const auto trace_path = dir / "trace.log";

    trace_event(trace_path, "coordinator_start",
        std::string("dir=") + dir.string() + " own_dir=" + (own_dir ? "1" : "0") +
        (existing_dir ? std::string(" existing_env=") + existing_dir : ""));

    if (own_dir) {
        std::ofstream truncate(log_path, std::ios::binary | std::ios::trunc);
        if (!truncate) {
            return 1;
        }
    }

#ifdef _WIN32
    if (own_dir) {
        _putenv_s(k_shared_dir_env, dir.string().c_str());
    }
#else
    if (own_dir) {
        setenv(k_shared_dir_env, dir.string().c_str(), 1);
    }
#endif

    trace_event(trace_path, "init.begin", "starting sintra runtime");
    sintra::init(argc, argv, worker);
    trace_event(trace_path, "init.end", "runtime initialized");

    // Wait for both workers to log receipt of the broadcast.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t seen = 0;
    size_t last_reported = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        seen = read_lines(log_path).size();
        if (seen != last_reported) {
            trace_event(trace_path, "coordinator_progress", "seen=" + std::to_string(seen));
            last_reported = seen;
        }
        if (seen >= 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (seen < 2) {
        const auto contents = read_lines(log_path);
        std::ostringstream oss;
        oss << "coordinator saw=" << seen << " log=[";
        for (size_t i = 0; i < contents.size(); ++i) {
            if (i) {
                oss << ", ";
            }
            oss << contents[i];
        }
        oss << "]";
        trace_event(trace_path, "coordinator_exit", oss.str());
    } else {
        trace_event(trace_path, "coordinator_exit", "seen=" + std::to_string(seen));
    }

    sintra::finalize();

    return seen >= 2 ? 0 : 1;
}
