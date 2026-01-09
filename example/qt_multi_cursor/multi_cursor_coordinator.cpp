/**
 * @file multi_cursor_coordinator.cpp
 * @brief Headless coordinator for the multi-cursor Qt example.
 *
 * This process:
 * - Spawns 4 window processes
 * - Monitors for normal exits via coordinator lifecycle callbacks
 * - Exits when all windows exit (normal or crash)
 *
 * Crash recovery is coordinated here with a 5-second countdown before respawn.
 */

#include "multi_cursor_common.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

// Coordinator state
struct Window_state {
    bool exited_normally = false;
    bool recovering = false;
    bool active = false;
};

std::mutex g_state_mutex;
std::condition_variable g_state_cv;
std::array<Window_state, sintra_example::k_num_windows> g_window_states;
std::unordered_map<uint64_t, int> g_process_index_to_window;
std::unordered_map<int, uint64_t> g_window_to_process_index;
std::unordered_map<uint64_t, sintra::process_lifecycle_event> g_pending_lifecycle_events;
sintra_example::Cursor_bus* g_bus = nullptr;

void broadcast_countdown(int window_id, int seconds_remaining);
void broadcast_spawned(int window_id);

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

bool all_windows_inactive()
{
    for (const auto& state : g_window_states) {
        if (state.active || state.recovering) {
            return false;
        }
    }
    return true;
}

bool all_windows_exited_normally()
{
    for (const auto& state : g_window_states) {
        if (!state.exited_normally) {
            return false;
        }
    }
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
        g_state_cv.notify_all();
    }
}

void mark_spawned(int window_id)
{
    if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
        return;
    }

    auto& state = g_window_states[window_id];
    state.exited_normally = false;
    state.recovering = false;
    if (!state.active) {
        state.active = true;
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
        g_state_cv.notify_all();
    }
}

struct Lifecycle_actions {
    int window_id = -1;
    int status = 0;
    bool notify_cursor_left = false;
    bool notify_normal_exit = false;
    bool log_crash = false;
    bool log_unpublished = false;
    bool log_normal_exit = false;
};

Lifecycle_actions apply_lifecycle_event_locked(
    int window_id,
    const sintra::process_lifecycle_event& event)
{
    Lifecycle_actions actions;
    actions.window_id = window_id;
    actions.status = event.status;

    auto& state = g_window_states[window_id];

    switch (event.why) {
    case sintra::process_lifecycle_event::reason::crash:
        state.recovering = false;
        mark_inactive(window_id);
        actions.notify_cursor_left = true;
        actions.log_crash = true;
        break;
    case sintra::process_lifecycle_event::reason::normal_exit:
        if (!state.exited_normally) {
            state.exited_normally = true;
            actions.notify_normal_exit = true;
            actions.log_normal_exit = true;
        }
        state.recovering = false;
        mark_inactive(window_id);
        break;
    case sintra::process_lifecycle_event::reason::unpublished:
        state.recovering = false;
        mark_inactive(window_id);
        if (!state.exited_normally) {
            actions.notify_cursor_left = true;
            actions.log_unpublished = true;
        }
        break;
    }

    return actions;
}

void dispatch_lifecycle_actions(const Lifecycle_actions& actions)
{
    if (actions.window_id < 0) {
        return;
    }

    if (actions.notify_cursor_left && g_bus) {
        g_bus->emit_remote<sintra_example::Cursor_bus::cursor_left>(actions.window_id);
    }
    if (actions.notify_normal_exit && g_bus) {
        g_bus->emit_remote<sintra_example::Cursor_bus::normal_exit_notification>(
            actions.window_id);
    }

    if (actions.log_crash) {
        sintra::Log_stream(sintra::log_level::info)
            << "[Coordinator] Window " << actions.window_id
            << " crashed (signal " << actions.status << ")\n";
    }
    else
    if (actions.log_unpublished) {
        sintra::Log_stream(sintra::log_level::info)
            << "[Coordinator] Window " << actions.window_id
            << " unpublished without normal-exit notice, treating as crash\n";
    }
    else
    if (actions.log_normal_exit) {
        sintra::Log_stream(sintra::log_level::info)
            << "[Coordinator] Window " << actions.window_id << " signaled normal exit\n";
    }
}

bool take_pending_lifecycle_locked(
    uint64_t process_index,
    sintra::process_lifecycle_event& event_out)
{
    auto it = g_pending_lifecycle_events.find(process_index);
    if (it == g_pending_lifecycle_events.end()) {
        return false;
    }
    event_out = it->second;
    g_pending_lifecycle_events.erase(it);
    return true;
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
        activate(&Coordinator_handler::on_window_goodbye,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));
    }

private:
    void on_window_hello(const sintra_example::Cursor_bus::window_hello& msg)
    {
        const auto process_index = sintra::get_process_index(msg.sender_instance_id);
        if (msg.window_id < 0 || msg.window_id >= sintra_example::k_num_windows) {
            return;
        }

        bool should_broadcast_spawned = false;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            update_window_mapping(msg.window_id, process_index);
            auto& state = g_window_states[msg.window_id];
            should_broadcast_spawned = state.recovering;
            state.recovering = false;
            state.exited_normally = false;
            mark_active(msg.window_id);
        }

        if (should_broadcast_spawned) {
            broadcast_spawned(msg.window_id);
        }

        sintra::Log_stream(sintra::log_level::info)
            << "[Coordinator] Window " << msg.window_id
            << " announced (process index " << process_index << ")\n";
    }

    void on_window_goodbye(const sintra_example::Cursor_bus::window_goodbye& msg)
    {
        const auto process_index = sintra::get_process_index(msg.sender_instance_id);
        if (msg.window_id < 0 || msg.window_id >= sintra_example::k_num_windows) {
            return;
        }

        Lifecycle_actions actions;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            update_window_mapping(msg.window_id, process_index);
            sintra::process_lifecycle_event event;
            event.process_iid = msg.sender_instance_id;
            event.process_slot = static_cast<uint32_t>(process_index);
            event.status = 0;
            event.why = sintra::process_lifecycle_event::reason::normal_exit;
            actions = apply_lifecycle_event_locked(msg.window_id, event);
        }

        dispatch_lifecycle_actions(actions);
    }
};

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

    sintra::set_recovery_policy([](const sintra::Crash_info& info) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_process_index_to_window.find(static_cast<uint64_t>(info.process_slot));
        if (it == g_process_index_to_window.end()) {
            if (all_windows_exited_normally()) {
                return sintra::Recovery_action::skip();
            }
            return sintra::Recovery_action::immediate();
        }

        const int window_id = it->second;
        auto& state = g_window_states[window_id];
        if (state.exited_normally) {
            state.recovering = false;
            return sintra::Recovery_action::skip();
        }

        state.recovering = true;
        return sintra::Recovery_action::delay_for(
            std::chrono::seconds(sintra_example::k_recovery_delay_seconds));    
    }, [](const sintra::Crash_info& info) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_process_index_to_window.find(static_cast<uint64_t>(info.process_slot));
        if (it == g_process_index_to_window.end()) {
            return all_windows_exited_normally();
        }
        const int window_id = it->second;
        auto& state = g_window_states[window_id];
        if (state.exited_normally) {
            state.recovering = false;
            return true;
        }
        return false;
    });

    sintra::set_recovery_tick_handler([](const sintra::Crash_info& info, int seconds_remaining) {
        int window_id = -1;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto it = g_process_index_to_window.find(static_cast<uint64_t>(info.process_slot));
            if (it == g_process_index_to_window.end()) {
                return;
            }
            window_id = it->second;
            auto& state = g_window_states[window_id];
            if (state.exited_normally) {
                state.recovering = false;
                return;
            }
            state.recovering = true;
        }

        broadcast_countdown(window_id, seconds_remaining);
    });

    sintra::set_lifecycle_handler([](const sintra::process_lifecycle_event& event) {
        Lifecycle_actions actions;
        bool has_actions = false;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto it = g_process_index_to_window.find(static_cast<uint64_t>(event.process_slot));
            if (it == g_process_index_to_window.end()) {
                g_pending_lifecycle_events[static_cast<uint64_t>(event.process_slot)] = event;
                return;
            }
            actions = apply_lifecycle_event_locked(it->second, event);
            has_actions = true;
        }

        if (has_actions) {
            dispatch_lifecycle_actions(actions);
        }
    });

    // Spawn 4 window processes
    const std::string window_path = get_window_path(argc > 0 ? argv[0] : "");
    sintra::Log_stream(sintra::log_level::info)
        << "[Coordinator] Spawning windows from: " << window_path << "\n";

    for (int i = 0; i < sintra_example::k_num_windows; ++i) {
        const auto piid = sintra::join_swarm(i + 1, window_path);
        if (piid == sintra::invalid_instance_id) {
            sintra::Log_stream(sintra::log_level::error)
                << "[Coordinator] Failed to spawn window " << i << "\n";
        }
        else {
            Lifecycle_actions pending_actions;
            bool has_pending_actions = false;
            const auto process_index = sintra::get_process_index(piid);
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                mark_spawned(i);
                update_window_mapping(i, process_index);
                sintra::process_lifecycle_event pending_event;
                if (take_pending_lifecycle_locked(process_index, pending_event)) {
                    pending_actions = apply_lifecycle_event_locked(i, pending_event);
                    has_pending_actions = true;
                }
            }
            if (has_pending_actions) {
                dispatch_lifecycle_actions(pending_actions);
            }
            sintra::Log_stream(sintra::log_level::info)
                << "[Coordinator] Spawned window " << i << "\n";
        }
    }

    // Wait for all windows to exit
    sintra::Log_stream(sintra::log_level::info)
        << "[Coordinator] Waiting for all windows to exit...\n";

    {
        std::unique_lock<std::mutex> lock(g_state_mutex);
        g_state_cv.wait(lock, [] {
            return all_windows_inactive();
        });
    }

    sintra::Log_stream(sintra::log_level::info)
        << "[Coordinator] All windows have exited, shutting down\n";

    sintra::finalize();

    return 0;
}
