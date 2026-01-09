/**
 * @file multi_cursor_coordinator.cpp
 * @brief Headless coordinator for the multi-cursor Qt example.
 *
 * This process:
 * - Spawns 4 window processes
 * - Monitors for normal exits via normal_exit_notification messages
 * - Exits when all windows exit normally
 *
 * Crash recovery is coordinated here with a 5-second countdown before respawn.
 */

#include "multi_cursor_common.h"

#include <sintra/detail/id_types.h>
#include <sintra/detail/process/coordinator.h>
#include <sintra/detail/process/managed_process.h>
#include <sintra/logging.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// Coordinator state
struct Window_state {
    bool exited_normally = false;
    bool recovering = false;
    bool active = false;
    bool seen = false;
    std::chrono::steady_clock::time_point spawned_at{};
};

std::mutex g_state_mutex;
std::condition_variable g_state_cv;
std::array<Window_state, sintra_example::k_num_windows> g_window_states;        
std::unordered_map<uint64_t, int> g_process_index_to_window;
std::unordered_map<int, uint64_t> g_window_to_process_index;
int g_active_count = 0;
std::atomic<bool> g_stop_monitor{false};

std::string get_window_path(const char* argv0)
{
    std::string exe_path(argv0 ? argv0 : "");
    size_t last_sep = exe_path.find_last_of("/\\");
    std::string dir = (last_sep != std::string::npos)
        ? exe_path.substr(0, last_sep + 1)
        : "";

#ifdef _WIN32
    return dir + "sintra_example_qt_multi_cursor_window.exe";
#else
    return dir + "sintra_example_qt_multi_cursor_window";
#endif
}

void update_window_mapping(int window_id, uint64_t process_index)
{
    auto existing = g_window_to_process_index.find(window_id);
    if (existing != g_window_to_process_index.end()) {
        g_process_index_to_window.erase(existing->second);
    }

    g_window_to_process_index[window_id] = process_index;
    g_process_index_to_window[process_index] = window_id;
}

bool all_windows_exited_normally()
{
    return g_active_count == 0;
}

bool parse_window_id_from_name(const std::string& assigned_name, int& window_id)
{
    const std::string prefix = "cursor_window_";
    if (assigned_name.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string suffix = assigned_name.substr(prefix.size());
    char* end = nullptr;
    const long parsed = std::strtol(suffix.c_str(), &end, 10);
    if (end == suffix.c_str() || *end != '\0') {
        return false;
    }
    if (parsed < 0 || parsed >= sintra_example::k_num_windows) {
        return false;
    }
    window_id = static_cast<int>(parsed);
    return true;
}

void mark_active(int window_id)
{
    if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
        return;
    }

    auto& state = g_window_states[window_id];
    if (!state.active) {
        state.active = true;
        ++g_active_count;
        g_state_cv.notify_all();
    }
}

void mark_spawned(int window_id)
{
    if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
        return;
    }

    auto& state = g_window_states[window_id];
    state.spawned_at = std::chrono::steady_clock::now();
    state.seen = false;
    state.exited_normally = false;
    if (!state.active) {
        state.active = true;
        ++g_active_count;
        g_state_cv.notify_all();
    }
}

void mark_inactive(int window_id)
{
    if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
        return;
    }

    auto& state = g_window_states[window_id];
    if (state.active) {
        state.active = false;
        if (g_active_count > 0) {
            --g_active_count;
        }
        g_state_cv.notify_all();
    }
}

// Transceiver for the coordinator to receive/send messages
class Coordinator_handler : public sintra_example::Cursor_bus
{
public:
    Coordinator_handler()
        : sintra_example::Cursor_bus("coordinator")
    {
        activate(&Coordinator_handler::on_window_hello,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));
        // Listen for normal exit notifications from any remote sender
        activate(&Coordinator_handler::on_normal_exit,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));
    }

private:
    void on_window_hello(const sintra_example::Cursor_bus::window_hello& msg)
    {
        const auto process_index = sintra::get_process_index(msg.sender_instance_id);
        if (msg.window_id < 0 || msg.window_id >= sintra_example::k_num_windows) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_state_mutex);
        update_window_mapping(msg.window_id, process_index);
        g_window_states[msg.window_id].recovering = false;
        g_window_states[msg.window_id].seen = true;
        mark_active(msg.window_id);

        sintra::Log_stream(sintra::log_level::info)
            << "[Coordinator] Window " << msg.window_id
            << " announced (process index " << process_index << ")\n";
    }

    void on_normal_exit(const sintra_example::Cursor_bus::normal_exit_notification& msg)
    {
        bool should_broadcast = false;
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (msg.window_id < 0 || msg.window_id >= sintra_example::k_num_windows) {
            return;
        }

        auto& state = g_window_states[msg.window_id];
        if (!state.exited_normally) {
            state.exited_normally = true;
            state.recovering = false;
            should_broadcast = true;
            sintra::Log_stream(sintra::log_level::info)
                << "[Coordinator] Window " << msg.window_id << " signaled normal exit\n";
        }

        mark_inactive(msg.window_id);
        g_state_cv.notify_all();

        if (should_broadcast) {
            emit_remote<sintra_example::Cursor_bus::normal_exit_notification>(msg.window_id);
        }
    }
};

Coordinator_handler* g_bus = nullptr;

void broadcast_countdown(int window_id, int seconds_remaining)
{
    if (!g_bus) {
        return;
    }
    g_bus->emit_remote<sintra_example::Cursor_bus::recovery_countdown>(
        window_id, seconds_remaining);
}

void broadcast_spawned(int window_id)
{
    if (!g_bus) {
        return;
    }
    g_bus->emit_remote<sintra_example::Cursor_bus::recovery_spawned>(window_id);
}

void spawn_window(int window_id, const std::string& window_path)
{
    std::vector<std::string> args = {
        window_path,
        "--window_id", std::to_string(window_id)
    };

    if (sintra::spawn_swarm_process(window_path, args) == 0) {
        sintra::Log_stream(sintra::log_level::error)
            << "[Coordinator] Failed to spawn window " << window_id << "\n";    
    }
    else {
        sintra::Log_stream(sintra::log_level::info)
            << "[Coordinator] Respawned window " << window_id << "\n";
        std::lock_guard<std::mutex> lock(g_state_mutex);
        mark_spawned(window_id);
    }
}

void start_recovery(int window_id, const std::string& window_path)
{
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (window_id < 0 || window_id >= sintra_example::k_num_windows) {      
            return;
        }

        auto& state = g_window_states[window_id];
        if (state.exited_normally || state.recovering || g_active_count == 0) {
            return;
        }
        state.recovering = true;
    }

    std::thread([window_id, window_path]() {
        for (int seconds = sintra_example::k_recovery_delay_seconds; seconds > 0; --seconds) {
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                if (g_active_count == 0 || g_window_states[window_id].exited_normally) {
                    g_window_states[window_id].recovering = false;
                    return;
                }
            }
            broadcast_countdown(window_id, seconds);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        spawn_window(window_id, window_path);
        broadcast_spawned(window_id);

        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_window_states[window_id].recovering = false;
        }
    }).detach();
}

} // namespace


int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, const_cast<const char* const*>(argv));
    }
    catch (const std::exception& e) {
        sintra::Log_stream(sintra::log_level::error)
            << "Failed to initialize sintra: " << e.what() << "\n";
        return 1;
    }

    sintra::Log_stream(sintra::log_level::info)
        << "[Coordinator] Starting multi-cursor coordinator\n";

    // Create our handler transceiver
    Coordinator_handler handler;
    g_bus = &handler;

    // Spawn 4 window processes
    const std::string window_path = get_window_path(argc > 0 ? argv[0] : "");   
    sintra::Log_stream(sintra::log_level::info)
        << "[Coordinator] Spawning windows from: " << window_path << "\n";      

    for (int i = 0; i < sintra_example::k_num_windows; ++i) {
        // Pass window_id as command line argument
        std::vector<std::string> args = {
            window_path,
            "--window_id", std::to_string(i)
        };

        if (sintra::spawn_swarm_process(window_path, args) == 0) {
            sintra::Log_stream(sintra::log_level::error)
                << "[Coordinator] Failed to spawn window " << i << "\n";        
        }
        else {
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                mark_spawned(i);
            }
            sintra::Log_stream(sintra::log_level::info)
                << "[Coordinator] Spawned window " << i << "\n";
        }
    }

    // Listen for crash notifications and delayed recovery
    sintra::activate_slot(
        [window_path](const sintra::Managed_process::terminated_abnormally& msg) {
            const auto process_index = sintra::get_process_index(msg.sender_instance_id);
            int window_id = -1;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                auto it = g_process_index_to_window.find(process_index);        
                if (it != g_process_index_to_window.end()) {
                    window_id = it->second;
                }
            }

            if (window_id < 0) {
                sintra::Log_stream(sintra::log_level::warning)
                    << "[Coordinator] Crash detected for unknown process index "
                    << process_index << "\n";
                return;
            }

            sintra::Log_stream(sintra::log_level::info)
                << "[Coordinator] Window " << window_id << " crashed (signal "
                << msg.status << "), starting recovery countdown\n";
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                mark_inactive(window_id);
            }
            if (g_bus) {
                g_bus->emit_remote<sintra_example::Cursor_bus::cursor_left>(window_id);
            }
            start_recovery(window_id, window_path);
        },
        sintra::Typed_instance_id<sintra::Managed_process>(sintra::any_remote));

    // Fallback: detect normal exits from coordinator unpublish events
    sintra::activate_slot(
        [window_path](const sintra::Coordinator::instance_unpublished& msg) {
            const std::string assigned = msg.assigned_name;
            int window_id = -1;
            if (!parse_window_id_from_name(assigned, window_id)) {
                return;
            }

            bool should_recover = false;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                auto& state = g_window_states[window_id];
                if (state.exited_normally) {
                    mark_inactive(window_id);
                    return;
                }
                if (state.recovering) {
                    mark_inactive(window_id);
                    return;
                }

                mark_inactive(window_id);
                should_recover = (g_active_count > 0);
            }

            if (should_recover) {
                sintra::Log_stream(sintra::log_level::info)
                    << "[Coordinator] Window " << window_id
                    << " unpublished without normal-exit notice, treating as crash\n";
                if (g_bus) {
                    g_bus->emit_remote<sintra_example::Cursor_bus::cursor_left>(window_id);
                }
                start_recovery(window_id, window_path);
            }
        },
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::any_local_or_remote));    

    // Monitor for missing window transceivers as a fallback for exit/crash detection
    std::thread presence_thread([window_path]() {
        constexpr auto k_spawn_grace = std::chrono::seconds(3);
        while (!g_stop_monitor.load()) {
            std::vector<int> to_recover;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                const auto now = std::chrono::steady_clock::now();
                for (int i = 0; i < sintra_example::k_num_windows; ++i) {
                    const auto iid = sintra::get_instance_id(
                        std::string(sintra_example::window_name(i)));
                    const bool exists = (iid != sintra::invalid_instance_id);
                    auto& state = g_window_states[i];

                    if (exists) {
                        state.seen = true;
                        mark_active(i);
                        continue;
                    }

                    if (state.active) {
                        if (!state.seen && (now - state.spawned_at) < k_spawn_grace) {
                            continue;
                        }

                        mark_inactive(i);
                        if (!state.exited_normally && !state.recovering && g_active_count > 0) {
                            to_recover.push_back(i);
                        }
                    }
                }
            }

            for (int window_id : to_recover) {
                if (g_bus) {
                    g_bus->emit_remote<sintra_example::Cursor_bus::cursor_left>(window_id);
                }
                start_recovery(window_id, window_path);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    // Wait for all windows to exit
    sintra::Log_stream(sintra::log_level::info)
        << "[Coordinator] Waiting for all windows to exit...\n";

    {
        std::unique_lock<std::mutex> lock(g_state_mutex);
        g_state_cv.wait(lock, [] {
            return all_windows_exited_normally();
        });
    }

    g_stop_monitor = true;
    if (presence_thread.joinable()) {
        presence_thread.join();
    }

    sintra::Log_stream(sintra::log_level::info)
        << "[Coordinator] All windows have exited, shutting down\n";

    sintra::finalize();

    return 0;
}
