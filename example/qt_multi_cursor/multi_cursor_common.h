#pragma once
/**
 * @file multi_cursor_common.h
 * @brief Shared message definitions for the Qt multi-cursor sync example.
 *
 * This example demonstrates:
 * - 4 windows that all send/receive cursor positions
 * - Each window has a unique color frame
 * - Cursor appears in receiver windows with the sender's color
 * - Crash detection and recovery with countdown
 * - Normal exit detection
 */
#include <sintra/sintra.h>

namespace sintra_example {

// Window IDs and colors
constexpr int k_num_windows = 4;
constexpr int k_recovery_delay_seconds = 5;

// Window colors (R, G, B)
constexpr int k_window_colors[k_num_windows][3] = {
    {220,  60,  60},   // Red
    { 60, 180,  60},   // Green
    { 60,  60, 220},   // Blue
    {220, 180,  60}    // Yellow/Gold
};

// Window names for discovery
inline const char* window_name(int window_id)
{
    static const char* names[] = {
        "cursor_window_0",
        "cursor_window_1",
        "cursor_window_2",
        "cursor_window_3"
    };
    return (window_id >= 0 && window_id < k_num_windows) ? names[window_id] : "cursor_window_unknown";
}

// Transceiver for cursor messages - each window has one
struct Cursor_bus : sintra::Derived_transceiver<Cursor_bus>
{
    using sintra::Derived_transceiver<Cursor_bus>::Derived_transceiver;

    // Sent by each window on startup so the coordinator can map process -> window
    SINTRA_MESSAGE(window_hello, int window_id);
    // Sent by a window before exit so the coordinator can count normal exits.
    SINTRA_MESSAGE(window_goodbye, int window_id);

    // Cursor position with source window info
    // x, y: cursor position
    // window_id: which window the cursor is in (determines color)
    SINTRA_MESSAGE(cursor_position, int x, int y, int window_id);

    // Cursor left a window, hide its ghost in other windows
    SINTRA_MESSAGE(cursor_left, int window_id);

    // Notification broadcast by the coordinator when a window exits normally
    // This allows other windows to distinguish normal exit from crash
    SINTRA_MESSAGE(normal_exit_notification, int window_id);

    // Sent by coordinator when a window crashes to synchronize countdown
    // seconds_remaining: time until respawn
    SINTRA_MESSAGE(recovery_countdown, int window_id, int seconds_remaining);

    // Sent by coordinator after a respawn attempt
    SINTRA_MESSAGE(recovery_spawned, int window_id);
};

} // namespace sintra_example
