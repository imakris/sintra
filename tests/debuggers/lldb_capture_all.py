"""
lldb script to capture comprehensive stack traces with local variables.

This script is executed by lldb to capture:
- All threads
- All stack frames (up to a limit)
- Function arguments
- Local variables
"""

import lldb
import sys


def capture_all_stacks(debugger, command, result, internal_dict):
    """Capture all stack traces with local variables for all threads."""
    target = debugger.GetSelectedTarget()
    if not target or not target.IsValid():
        print("ERROR: No valid target", file=sys.stderr)
        return

    process = target.GetProcess()
    if not process or not process.IsValid():
        print("ERROR: No valid process", file=sys.stderr)
        return

    num_threads = process.GetNumThreads()
    max_frames_per_thread = 256

    print(f"========================================")
    print(f"Process: PID {process.GetProcessID()}")
    print(f"Threads: {num_threads}")
    print(f"========================================\n")

    for thread_idx in range(num_threads):
        thread = process.GetThreadAtIndex(thread_idx)
        if not thread.IsValid():
            continue

        thread_id = thread.GetThreadID()
        thread_name = thread.GetName() or "(unnamed)"
        num_frames = thread.GetNumFrames()

        print(f"\n{'='*80}")
        print(f"Thread #{thread_idx} (ID: {thread_id}, Name: {thread_name})")
        print(f"Frames: {num_frames}")
        print(f"{'='*80}\n")

        # Capture up to max_frames_per_thread frames
        frames_to_capture = min(num_frames, max_frames_per_thread)
        for frame_idx in range(frames_to_capture):
            frame = thread.GetFrameAtIndex(frame_idx)
            if not frame.IsValid():
                continue

            function = frame.GetFunction()
            symbol = frame.GetSymbol()
            module = frame.GetModule()

            # Get function name
            if function and function.IsValid():
                func_name = function.GetName()
            elif symbol and symbol.IsValid():
                func_name = symbol.GetName()
            else:
                func_name = "<unknown>"

            # Get module name
            module_name = module.GetFileSpec().GetFilename() if module and module.IsValid() else "<unknown>"

            # Get line info
            line_entry = frame.GetLineEntry()
            if line_entry and line_entry.IsValid():
                file_spec = line_entry.GetFileSpec()
                line_num = line_entry.GetLine()
                file_name = file_spec.GetFilename() if file_spec else "<unknown>"
                location = f"{file_name}:{line_num}"
            else:
                location = "<no debug info>"

            print(f"  Frame {frame_idx}: {func_name}")
            print(f"    Module: {module_name}")
            print(f"    Location: {location}")
            print(f"    PC: {hex(frame.GetPC())}")

            # Capture arguments
            args = frame.GetVariables(True, False, False, False)  # arguments only
            if args.GetSize() > 0:
                print(f"    Arguments ({args.GetSize()}):")
                for i in range(args.GetSize()):
                    var = args.GetValueAtIndex(i)
                    if var and var.IsValid():
                        name = var.GetName()
                        value = var.GetValue()
                        type_name = var.GetTypeName()
                        # Try to get summary if value is None
                        if value is None:
                            value = var.GetSummary() or "<unavailable>"
                        print(f"      {name}: {type_name} = {value}")

            # Capture local variables
            locals_vars = frame.GetVariables(False, True, False, False)  # locals only
            if locals_vars.GetSize() > 0:
                print(f"    Local Variables ({locals_vars.GetSize()}):")
                for i in range(locals_vars.GetSize()):
                    var = locals_vars.GetValueAtIndex(i)
                    if var and var.IsValid():
                        name = var.GetName()
                        value = var.GetValue()
                        type_name = var.GetTypeName()
                        # Try to get summary if value is None
                        if value is None:
                            value = var.GetSummary() or "<unavailable>"
                        # Limit very long values
                        value_str = str(value)
                        if len(value_str) > 200:
                            value_str = value_str[:200] + "..."
                        print(f"      {name}: {type_name} = {value_str}")

            print()  # Blank line between frames

        if num_frames > max_frames_per_thread:
            print(f"  ... {num_frames - max_frames_per_thread} more frames omitted\n")


def __lldb_init_module(debugger, internal_dict):
    """Initialize the script when loaded by lldb."""
    debugger.HandleCommand(
        'command script add -f lldb_capture_all.capture_all_stacks capture_all_stacks'
    )
    print('The "capture_all_stacks" command has been installed.', file=sys.stderr)
