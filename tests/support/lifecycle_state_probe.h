#pragma once

#include <sintra/sintra.h>

#include <array>

struct Lifecycle_state_snapshot
{
    std::array<const void*, 14> addresses{};
    decltype(&sintra::s_signal_handler) signal_handler = nullptr;
    sintra::Managed_process* process = nullptr;
    int exit_code = 0;
    int timeout_ms = 0;
    bool disabled = false;
    bool dispatcher_ready = false;
    bool generation_active = false;
    uintptr_t transport = 0;
};

// Keep the probe local to each translation unit: an external inline probe could
// itself be coalesced and conceal duplicate runtime state.
static Lifecycle_state_snapshot capture_lifecycle_state()
{
    Lifecycle_state_snapshot state;
    state.addresses = {
        &sintra::pending_signal_mask(),
        &sintra::dispatched_signal_counter(),
        &sintra::signal_dispatcher_once_flag(),
        &sintra::signal_handler_once_flag(),
        &sintra::signal_slots(),
        &sintra::lifeline_shutdown_flag(),
        &sintra::s_lifeline_handle_value,
        &sintra::s_lifeline_exit_code,
        &sintra::s_lifeline_timeout_ms,
        &sintra::s_lifeline_disabled,
        &sintra::dispatch_shutdown_mutex_instance,
#ifdef _WIN32
        &sintra::signal_event(),
        &sintra::windows_signal_dispatch_generation(),
        nullptr
#else
        &sintra::alt_stack_storage(),
        &sintra::alt_stack_installed(),
        &sintra::signal_pipe()
#endif
    };
    state.signal_handler = &sintra::s_signal_handler;
    state.process = sintra::s_mproc;
    state.exit_code = sintra::s_lifeline_exit_code;
    state.timeout_ms = sintra::s_lifeline_timeout_ms;
    state.disabled = sintra::s_lifeline_disabled;
#ifdef _WIN32
    state.dispatcher_ready = sintra::signal_event() != nullptr;
    state.transport = reinterpret_cast<uintptr_t>(sintra::signal_event());
    state.generation_active = sintra::windows_signal_generation_is_active(
        sintra::windows_signal_dispatch_generation().load());
#else
    state.dispatcher_ready =
        sintra::signal_pipe()[0] >= 0 && sintra::signal_pipe()[1] >= 0;
    state.transport = static_cast<uintptr_t>(sintra::signal_pipe()[0]);
#endif
    return state;
}

Lifecycle_state_snapshot capture_peer_lifecycle_state();
void initialize_peer_lifecycle(int argc, char* argv[]);
bool shutdown_peer_lifecycle();
