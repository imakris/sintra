# Windows CI crash diagnostics overview

When the Sintra test runner reports a line such as
`CRASH: Process terminated abnormally (exit code 3221225477)` it has already
identified that Windows terminated the test process with an NTSTATUS value. On
Windows, return code `0xC0000005` (`3221225477` in decimal) is the access
violation status code that the operating system reports for an actual crash,
not a generic failure or early exit.

## Why the live stack capture is empty

After a crash the Python runner tries to attach the Windows debugger to the
failed process to capture the stack of the process and of any helpers in the
same process tree.【F:tests/run_tests.py†L2159-L2249】 The attachment is attempted
with the `-pv -p <pid>` options so that the debugger pauses the process and
prints every thread’s stack trace. In the failure you observed, the debugger
output shows `*** wait with pending attach` followed by `Unable to examine
process id …, HRESULT 0x80004002`. That HRESULT is returned when the process has
already exited, so there is nothing left for the debugger to attach to. By the
time the runner launches the debugger the process has fully terminated, which
means no live stack frames are available to display.

## Why no crash dump is found

When a crash is detected on Windows the runner also searches for a recent
`.dmp` file that matches the crashed test binary name and PID.【F:tests/run_tests.py†L2009-L2120】
The search now prioritizes `%LOCALAPPDATA%\sintra\CrashDumps`, a directory the
runner configures during start-up by enabling Windows Error Reporting “Local
Dump” support in the current user hive.【F:tests/run_tests.py†L1976-L2042】 If the
registry call fails (for example when the `reg.exe` tool is missing or the user
hive is read-only) the fallback search includes the working directory, the test
executable directory, and `%LOCALAPPDATA%\CrashDumps`. In that failure mode the
script still emits `[Post-mortem stack capture unavailable: no recent crash dump
found]` because the operating system never produced a dump.

## Where the crash dumps end up

Successful registry configuration makes Windows drop full-memory crash dumps in
`%LOCALAPPDATA%\sintra\CrashDumps`. The runner requests a maximum of ten recent
dumps to avoid filling the disk and uses the cached Debugging Tools to print all
thread stacks once a dump becomes available.【F:tests/run_tests.py†L1976-L2150】 If
you need to archive the dumps for offline analysis, collect them from that
directory before the CI job cleans up the workspace.

## Was this a real crash?

Yes. The exit code that triggered the `CRASH:` classification comes directly
from the terminated process and the runner only uses that path for signal-like
return values or Windows NTSTATUS codes.【F:tests/run_tests.py†L605-L645】 A
return code of `0xC0000005` cannot be produced by a normal `return` or `exit()`
call from the test, so it reflects an access violation raised by the operating
system.

To gather a post-mortem stack trace on Windows CI the runner now enables crash
dump creation automatically, but if you override the registry defaults or run
the tests outside the harness make sure the `Windows Error Reporting\LocalDumps`
keys still point to a writable location so that the dumps are produced.
