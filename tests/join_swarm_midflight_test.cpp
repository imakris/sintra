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

constexpr const char* kSharedDirEnv = "SINTRA_JOIN_SWARM_DIR";

struct Hello {};

std::filesystem::path shared_dir()
{
    const char* value = std::getenv(kSharedDirEnv);
    if (!value || !*value) {
        throw std::runtime_error("SINTRA_JOIN_SWARM_DIR is not set");
    }
    return std::filesystem::path(value);
}

void append_line(const std::filesystem::path& file, const std::string& line)
{
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string());
    }
    out << line << '\n';
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

    auto hello_slot = [&](Hello) {
        append_line(log_path, std::to_string(sintra::process_of(s_mproc_id)));
    };
    sintra::activate_slot(hello_slot);

    trace_event(trace_path, "worker_start",
        "sintra_process=" + std::to_string(sintra::process_of(s_mproc_id)) +
        " dir=" + dir.string());

    const bool initiator = std::filesystem::create_directory(initiator_marker);
    trace_event(trace_path, "role",
        std::string("sintra_process=") + std::to_string(sintra::process_of(s_mproc_id)) +
        (initiator ? " initiator" : " follower"));

    if (initiator) {
        trace_event(trace_path, "join_swarm.begin", "requesting additional process");
        const auto joined = sintra::join_swarm(1);
        trace_event(trace_path, "join_swarm.end", "joined=" + std::to_string(joined));
        if (joined == sintra::invalid_instance_id) {
            return 1;
        }
    }

    // Ensure both the existing worker and the newly joined worker rendezvous.
    trace_event(trace_path, "barrier.enter", "post-join-sync");
    sintra::barrier("post-join-sync", "_sintra_external_processes");
    trace_event(trace_path, "barrier.exit", "post-join-sync");

    if (initiator) {
        trace_event(trace_path, "broadcast", "coordinator broadcasting Hello");
        sintra::world() << Hello{};
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
    const char* existing_dir = std::getenv(kSharedDirEnv);
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
        _putenv_s(kSharedDirEnv, dir.string().c_str());
    }
#else
    if (own_dir) {
        setenv(kSharedDirEnv, dir.string().c_str(), 1);
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
