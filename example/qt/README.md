# Qt Cursor Sync Example

This example demonstrates how to bridge Qt signals with sintra messages.
The sender emits a Qt signal on mouse movement, forwards it to sintra,
and the receiver activates a handler for the named sender instance, dispatching
back to the Qt UI thread.

## Build

Configure with Qt and enable the Qt examples:

```cmd
cmake -S . -B <build_dir> -DSINTRA_BUILD_QT_EXAMPLES=ON -DCMAKE_PREFIX_PATH=<qt_prefix>
cmake --build <build_dir> --config <config>
```

## Run

Start the sender; it will spawn the receiver:

```cmd
<build_dir>\example\qt\<config>\sintra_example_qt_cursor_sync_sender.exe
```

On Linux/macOS:

```bash
./<build_dir>/example/qt/sintra_example_qt_cursor_sync_sender
```
