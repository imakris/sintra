# Sintra

[![Build - Linux](https://github.com/imakris/sintra/actions/workflows/build-linux.yml/badge.svg?branch=master)](https://github.com/imakris/sintra/actions/workflows/build-linux.yml)
[![Stress Test - Linux](https://img.shields.io/github/check-runs/imakris/sintra/master?nameFilter=Stress%20Test%20-%20Linux)](https://github.com/imakris/sintra/actions/workflows/stress-test-linux.yml)

[![Build - macOS](https://github.com/imakris/sintra/actions/workflows/build-macos.yml/badge.svg?branch=master)](https://github.com/imakris/sintra/actions/workflows/build-macos.yml)
[![Stress Test - macOS](https://img.shields.io/github/check-runs/imakris/sintra/master?nameFilter=Stress%20Test%20-%20macOS)](https://github.com/imakris/sintra/actions/workflows/stress-test-macos.yml)

[![Build - Windows](https://github.com/imakris/sintra/actions/workflows/build-windows.yml/badge.svg?branch=master)](https://github.com/imakris/sintra/actions/workflows/build-windows.yml)
[![Stress Test - Windows](https://img.shields.io/github/check-runs/imakris/sintra/master?nameFilter=Stress%20Test%20-%20Windows)](https://github.com/imakris/sintra/actions/workflows/stress-test-windows.yml)

[![FreeBSD Build & Test](https://api.cirrus-ci.com/github/imakris/sintra.svg?task=FreeBSD%20Build%20%26%20Test&branch=master)](https://cirrus-ci.com/github/imakris/sintra)


![Header-only](https://img.shields.io/badge/header--only-yes-lightgrey)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-green)

Sintra is a C++ library for building type-safe interprocess communication layers.
It provides a common language for sending signals, broadcasting events, and invoking
remote procedures across process boundaries without resorting to fragile string-based
protocols. The library focuses on expressiveness and safety, making it easier to
coordinate modular applications, daemons, and tools that need to communicate reliably.

## Key features

* **Type-safe APIs across processes** â€“ interfaces are expressed as C++ types, so
  mismatched payloads are detected at compile time instead of surfacing as runtime
  protocol errors.
* **Signal bus and RPC in one package** â€“ publish/subscribe message dispatch and
  synchronous remote procedure calls share the same primitives, allowing you to mix
  patterns as your architecture requires.
* **Header-only distribution** â€“ integrate the library by adding the headers to your
  project; no separate build step or binaries are necessary.
* **Cross-platform design** â€“ built on top of Boost.Interprocess and related
  header-only Boost utilities, enabling shared-memory transport on Linux and Windows.
* **Opt-in crash recovery** â€“ mark critical workers with `sintra::enable_recovery()` so
  the coordinator automatically respawns them after an unexpected exit.

Typical use cases include plugin hosts coordinating work with out-of-process plugins,
GUI front-ends that need to communicate with background services, and distributed test
harnesses that must keep multiple workers in sync while exchanging strongly typed data.


## Interprocess Communication Patterns

### Broadcast a Ping and listen from another process

```cpp
// Sender process: announce a shared struct Ping to everyone listening.
sintra::world() << Ping{};

// Receiver process: register a slot so cross-process Pings show up locally.
sintra::activate_slot([](const Ping&) {
    sintra::console() << "Received Ping from another process" << '\n';
});
```

### Export a transceiver method for RPC

```cpp
struct Remotely_accessible: sintra::Derived_transceiver<Remotely_accessible>
{
    std::string append(const std::string& s, int v) {
        return std::to_string(v) + ": " + s;
    }

    SINTRA_RPC(append); // generates Remotely_accessible::rpc_append(...)
};
```

### Handle a Remote Exception

```cpp
// Remote exceptions thrown inside append() propagate back across the process boundary.
try {
    sintra::console() << Remotely_accessible::rpc_append("instance", "Hi", 42) << '\n';
}
catch (const std::exception& e) {
    sintra::console() << "Remote RPC failed in callee: " << e.what() << '\n';
}
```

### Observe abnormal exits from managed peers

```cpp
sintra::activate<sintra::Managed_process>(
    [](const sintra::Managed_process::terminated_abnormally& crash) {
        sintra::console()
            << "Process "
            << sintra::process_of(crash.sender_instance_id)
            << " crashed with status " << crash.status << '\n';
    },
    sintra::any_remote);
```


## Threading Model and Barriers

### Asynchronous Message Dispatch

Sintra uses **dedicated reader threads** to process incoming messages from shared memory rings. When a message arrives:
1. A reader thread pulls the message from the ring buffer.
2. The reader thread invokes the matching slot or RPC handler **asynchronously**.
3. Handlers (and their post-handler continuations) execute on the reader thread, not the thread that published the message or called the barrier.

**Concurrency reminder:** Slot handlers that touch shared state must still synchronize with other threads in your process (via mutexes, atomics, etc.). The barriers described below coordinate *when* handlers run; they do not eliminate the need for thread-safe data structures.

### Barrier Semantics

`sintra::barrier()` coordinates progress across processes and comes in three flavors that trade off strength for cost. The template defaults to `delivery_fence_t`, so a plain `barrier("name")` is already stronger than a bare rendezvous.

| Mode | Call form | What it guarantees when the function returns |
| --- | --- | --- |
| **Rendezvous** | `barrier<sintra::rendezvous_t>(name)` | Every participant has reached the barrier. No guarantees about outstanding messages. |
| **Delivery fence (default)** | `barrier(name)` or `barrier<sintra::delivery_fence_t>(name)` | All pre-barrier messages have been **fetched** by the local reader thread and are queued for handling. Their handlers may still be running. |
| **Processing fence** | `barrier(sintra::processing_fence_t{}, name)` | All handlers for messages published before the barrier have **completed** (including per-handler continuations) on every participant. |

Delivery fences cost the same as rendezvous plus a short wait for readers to catch up. Processing fences add a single control message per process and an extra rendezvous so you can observe handler side effects deterministically.

```cpp
// Wait until everyone reaches the same point and any prior messages are queued locally.
sintra::barrier("phase-1"); // delivery fence

// Later: ensure the side effects from earlier messages are visible before reading shared data.
sintra::barrier(sintra::processing_fence_t{}, "apply-updates");
```

Processing fences are safe to call from any thread, including handlers themselves: if the current thread is the reader, the fence returns immediately. When coordination between threads inside the same process is also required, combine Sintra barriers with standard threading primitives.

## Getting started

1. Add the `include/` directory to your project's include path.
2. Ensure that a C++17 compliant compiler is used (GCC, Clang, or MSVC are supported).
3. Sintra vendors the required Boost headers under `third_party/boost`,
   including `interprocess`, `type_index`, and `fusion`, so no additional
   dependency setup is needed when using the repository as-is.
4. Explore the `example/` directory to see how to set up signal buses, channels, and
   remote call endpoints.

Because everything ships as headers, Sintra works well in monorepos or projects that
prefer vendoring dependencies as git submodules or fetching them during configuration.

## Platform requirements

* **macOS** – Sintra always uses `os_sync_wait_on_address` for its interprocess semaphore implementation. The build fails if `<os/os_sync_wait_on_address.h>` or `<os/clock.h>` is missing, so ensure the runner has macOS 13 or newer with the Xcode 15 (or newer) Command Line Tools installed. No legacy semaphore fallback is provided or supported.

## Tests and continuous integration

The library ships with a small suite of integration tests that exercise the
publish/subscribe bus, ping-pong channels (single and multi-producer), remote
procedure calls, and the managed-process recovery path. They are defined in the
`tests/` directory and can be executed locally by configuring the project with
`cmake` and running `ctest` from the build tree. For longer stress runs that
mirror the CI configuration, invoke

```
python tests/run_tests.py --repetitions 10 --timeout 30 --build-dir build --config Release
```

after building; the script repeatedly launches the compiled test binaries and
handles timeouts or stalled processes. Add `--preserve-stalled-processes` if you
need to keep wedged helpers alive for debugging instead of terminating them.

### Operational guidance for `spawn_detached`

The `sintra::spawn_detached` helper launches helper processes by way of a
double-fork sequence and an anonymous pipe handshake. When the helper cannot
create the pipe (for example because all file descriptors are exhausted) or is
unable to report readiness through the pipe, the function now returns `false`.
Callers should treat a `false` return value as a hard failure, log the error, and
optionally retry after freeing resources. The handshake guarantees that a
successful return means the grandchild has executed `execv` and relinquished the
pipe, so spurious successes caused by early crashes are prevented.

Continuous integration runs on both Linux and Windows through GitHub Actions.
Each build workflow compiles the project in Release mode and publishes the
artifacts. A follow-up stress-test workflow triggers when the build completes,
downloads the artifacts, makes the bundled test executables runnable, and then
executes `tests/run_tests.py --repetitions 10 --timeout 30 --kill_stalled_processes`
to shake out intermittent issues.

## License

The source code is licensed under the Simplified BSD License.
