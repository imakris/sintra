# Qt Multi-Cursor Example

This example demonstrates advanced Sintra features using Qt:

- **4 windows** that all act as both senders and receivers
- **Cursor replication** across windows with color-coded source indication
- **Crash detection and coordinator-driven recovery** with 5-second countdown
- **Normal exit detection** with notifications to other windows
- **Separate coordinator process** that manages the window lifecycle

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        COORDINATOR PROCESS                          │
│   - Headless (no window)                                            │
│   - Spawns 4 window processes                                       │
│   - Waits for all windows to exit normally                          │
│   - Crash recovery coordinated here (5s delay)                      │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ spawn_swarm_process()
           ┌───────────────┼───────────────┬───────────────┐
           │               │               │               │
           ▼               ▼               ▼               ▼
    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
    │ Window 0 │    │ Window 1 │    │ Window 2 │    │ Window 3 │
    │  (Red)   │◄──►│ (Green)  │◄──►│  (Blue)  │◄──►│ (Yellow) │
    └──────────┘    └──────────┘    └──────────┘    └──────────┘
         ▲               ▲               ▲               ▲
         └───────────────┴───────────────┴───────────────┘
                    Cursor position messages
```

## Features

### Cursor Replication
- Move the mouse over any window to see the cursor appear in all other windows
- The ghost cursor is drawn in the color of the originating window
- Each ghost cursor shows a small number indicating its source window

### Crash Recovery
- Each window has a red "Crash" button that deliberately crashes the process
- When a window crashes:
  1. The coordinator receives `terminated_abnormally` from Sintra
  2. The coordinator broadcasts a synchronized 5-second countdown
  3. After the delay, the coordinator respawns the crashed window
  4. When cursor updates resume, windows show a "recovered" message
  5. The recovered window rejoins the swarm seamlessly

### Normal Exit Handling
- If you close a window normally (click X), it sends a `normal_exit_notification`
- The coordinator rebroadcasts the normal exit so every window stays in sync
- Other windows display "Window N exited" in the notifications area
- The closed window is NOT restarted (unlike crash recovery)
- When all windows exit (normal or crash), the coordinator also exits

## Building

```bash
# Configure with Qt examples enabled
cmake -S . -B build -DSINTRA_BUILD_QT_EXAMPLES=ON -DCMAKE_PREFIX_PATH=<path-to-qt>

# Build
cmake --build build --config Release

# On Windows, the executables will be in:
# build/example/qt_multi_cursor/Release/sintra_example_qt_multi_cursor_coordinator.exe
# build/example/qt_multi_cursor/Release/sintra_example_qt_multi_cursor_window.exe
```

## Running

Start the coordinator process - it will automatically spawn all 4 windows:      

```bash
# Windows
build\example\qt_multi_cursor\Release\sintra_example_qt_multi_cursor_coordinator.exe

# Linux/macOS
./build/example/qt_multi_cursor/sintra_example_qt_multi_cursor_coordinator      
```

On Windows, you can also use the helper batch files:

```bat
example\qt_multi_cursor\build_multi_cursor.bat Release
example\qt_multi_cursor\run_multi_cursor.bat Release
```

## Code Structure

- `multi_cursor_common.h` - Shared message definitions and constants
- `multi_cursor_coordinator.cpp` - Headless coordinator that manages windows
- `multi_cursor_window.cpp` - Qt window with cursor tracking and crash button

## Sintra Features Demonstrated

1. **Named Transceivers**: Each window registers with a unique name (`cursor_window_0`, etc.)
2. **Remote Message Emission**: `emit_remote<>()` sends messages to all other processes
3. **Wildcard Message Handlers**: `activate()` with `Typed_instance_id<T>(any_remote)` receives from any remote sender
4. **Crash Signals**: `Managed_process::terminated_abnormally` notifies the coordinator about crashes
5. **Coordinator Broadcasts**: countdown and respawn notifications keep windows synchronized
6. **Thread-safe UI Updates**: `post_to_ui()` helper using `QMetaObject::invokeMethod()` for safe Qt updates from Sintra handlers

## Message Flow

### Cursor Updates
```
Window A (mouse move) -> emit_remote<cursor_position>(x, y, window_id)
                      -> All other windows receive via on_cursor_message()
                      -> Ghost cursor drawn in Window A's color
```

### Normal Exit
```
Window A (close button) -> emit_remote<normal_exit_notification>(window_id)
                        -> All other windows receive
                        -> Displayed as "Window N exited"
                        -> Ghost cursor hidden, no recovery expected
```

### Crash & Recovery
```
Window A (crash button) -> Process crashes (null pointer dereference)
                        -> Coordinator receives terminated_abnormally
                        -> Coordinator broadcasts countdown (5..1)
                        -> Coordinator respawns Window A after delay
                        -> Recovered Window A starts sending cursor updates
                        -> Other windows show "Window N recovered!"
```
