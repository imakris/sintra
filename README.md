# Sintra

<table>
  <thead>
    <tr>
      <th>Platform</th>
      <th>Build</th>
      <th>Tests</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Linux</td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/build-linux.yml"><img alt="Linux Build" src="https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/imakris/38c12e638eddbbfad6962730f6b10d20/raw/sintra-linux-build.json" style="display:block;margin:0 auto;"></a></td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/build-linux.yml"><img alt="Linux Tests" src="https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/imakris/38c12e638eddbbfad6962730f6b10d20/raw/sintra-linux-tests.json" style="display:block;margin:0 auto;"></a></td>
    </tr>
    <tr>
      <td>macOS</td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/build-macos.yml"><img alt="macOS Build" src="https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/imakris/38c12e638eddbbfad6962730f6b10d20/raw/sintra-macos-build.json" style="display:block;margin:0 auto;"></a></td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/build-macos.yml"><img alt="macOS Tests" src="https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/imakris/38c12e638eddbbfad6962730f6b10d20/raw/sintra-macos-tests.json" style="display:block;margin:0 auto;"></a></td>
    </tr>
    <tr>
      <td>Windows</td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/build-windows.yml"><img alt="Windows Build" src="https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/imakris/38c12e638eddbbfad6962730f6b10d20/raw/sintra-windows-build.json" style="display:block;margin:0 auto;"></a></td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/build-windows.yml"><img alt="Windows Tests" src="https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/imakris/38c12e638eddbbfad6962730f6b10d20/raw/sintra-windows-tests.json" style="display:block;margin:0 auto;"></a></td>
    </tr>
    <tr>
      <td>FreeBSD</td>
      <td colspan="2" style="text-align:center;"><a href="https://cirrus-ci.com/github/imakris/sintra"><img alt="FreeBSD Build &amp; Test" src="https://api.cirrus-ci.com/github/imakris/sintra.svg?task=FreeBSD%20Build%20%26%20Test&amp;branch=master&amp;v=2" style="display:block;margin:0 auto;"></a></td>
    </tr>
  </tbody>
</table>

<table>
  <thead>
    <tr>
      <th colspan="2">Coverage</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Linux (gcc)</td>
      <td style="text-align:center;"><img alt="Codecov GCC" src="https://codecov.io/gh/imakris/sintra/branch/master/graph/badge.svg?flag=gcc" style="display:block;margin:0 auto;"></td>
    </tr>
    <tr>
      <td>Linux (clang)</td>
      <td style="text-align:center;"><img alt="Codecov Clang" src="https://codecov.io/gh/imakris/sintra/branch/master/graph/badge.svg?flag=clang" style="display:block;margin:0 auto;"></td>
    </tr>
  </tbody>
</table>


![Header-only](https://img.shields.io/badge/header--only-yes-lightgrey)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-green)

Sintra is a C++17 library for type-safe interprocess communication on a single host.
It lets independent processes exchange typed messages, broadcast events, and invoke
RPC-style calls with a compile-time-checked API, avoiding string-based protocols and
external brokers. It also provides coordination primitives such as named barriers,
as well as typed publish/subscribe, synchronous RPC, crash detection, and opt-in worker respawning.

Sintra targets low-latency, crash-resilient local IPC where shared-memory transport and
coordination need to be integrated rather than assembled from multiple layers. Common
alternatives such as ZeroMQ or nanomsg provide local transports, but those are socket-based,
cross the kernel boundary, and still copy data. Sintra uses memory-mapped shared rings
so data stays in user space and readers access published messages directly, which is
suitable for latency-sensitive workloads.

## Table of contents

- [Key features](#key-features)
- [Quick example](#quick-example)
- [Getting started](#getting-started)
- [Supported platforms and architectures](#supported-platforms-and-architectures)
- [Interprocess communication patterns](#interprocess-communication-patterns)
- [Advanced topics](#advanced-topics)
- [Tests and continuous integration](#tests-and-continuous-integration)
- [License](#license)


## Key features

* **Type-safe APIs across processes** - interfaces are expressed as C++ types, so
  mismatched payloads are detected at compile time instead of surfacing as runtime
  protocol errors.
* **Signal bus and RPC in one package** - publish/subscribe message dispatch and
  synchronous remote procedure calls share the same primitives, allowing programs to mix
  patterns as the architecture requires.
* **Header-only distribution** - integrate the library by adding the headers to a
  project; no separate build step or binaries are necessary.
* **No RTTI required** - type ids are derived from compile-time signatures (or
  explicit ids when pinned).
* **Cross-platform design** - shared-memory transport on Linux, macOS, Windows, and FreeBSD.
* **Opt-in crash recovery** - mark critical workers with `sintra::enable_recovery()` so
  the coordinator automatically respawns them after an unexpected exit.
* **Lifeline ownership for spawned processes** - child processes monitor a lifeline
  pipe/handle and hard-exit if the owner disappears (timeout and exit code are configurable).

Typical use cases include plugin hosts coordinating work with out-of-process plugins,
GUI front-ends that need to communicate with background services, and distributed test
harnesses that must keep multiple workers in sync while exchanging strongly typed data.

## Quick example

```cpp
// Publisher process: announce a shared struct Ping to everyone listening.
sintra::world() << Ping{};

// Receiver process: register a slot so cross-process Pings show up locally.
sintra::activate_slot([](const Ping&) {
    sintra::console() << "Received Ping from another process" << '\n';
});
```

## Getting started

1. The `include/` directory must be on the project's include path.
2. A C++17 compliant compiler is required (GCC, Clang, or MSVC are supported).
3. The `example/` directory contains signal bus, channel, and remote call samples.

Because everything ships as headers, Sintra works well in monorepos or projects that
prefer vendoring dependencies as git submodules or fetching them during configuration.

## Supported platforms and architectures

* **Linux, macOS, Windows, FreeBSD** - shared-memory transport is supported on all four.
* **CPU architectures** - Sintra targets x86/x64 and ARM/AArch64 CPUs. Builds on other
  architectures still succeed, but they emit a warning and fall back to a simple no-op
  spin pause for the interprocess primitives, so those primitives may run with a very
  basic implementation and performance is not guaranteed.
* **macOS requirement** - Sintra always uses `os_sync_wait_on_address` for its interprocess
  semaphore implementation. The build fails if `<os/os_sync_wait_on_address.h>` or
  `<os/clock.h>` is missing, so runners should use macOS 15.0 or newer with the Command
  Line Tools for Xcode 15 (or newer) installed (the full Xcode IDE is not required). Older
  macOS versions are not supported.


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

### Send a targeted fire-and-forget message

```cpp
struct Unicast_receiver : sintra::Derived_transceiver<Unicast_receiver>
{
    void handle_unicast(const Ping& msg) {
        sintra::console() << "Got targeted ping\n";
    }

    SINTRA_UNICAST(handle_unicast)
};

// Send to a specific instance id (e.g., exchanged out-of-band or via a broadcast).
Unicast_receiver::rpc_handle_unicast(target_instance_id, Ping{});
```

For full flows, see `example/sintra/sintra_example_0_basic_pubsub.cpp`,
`example/sintra/sintra_example_6_unicast_send_to.cpp`, and
`example/sintra/sintra_example_2_rpc_append.cpp`.

### Block until a specific message arrives

```cpp
// Wait for a Stop signal (synchronous receive).
sintra::receive<Stop>();

// Wait for a message and capture its payload.
auto msg = sintra::receive<DataMessage>();
sintra::console() << "value=" << msg.value << '\n';
```

Note: call `receive<T>()` from main/control threads only; do not call it from a message handler.
Debug builds abort if this is violated.

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


Usage example:

```cpp
// Callee process: create and name the instance.
Remotely_accessible ra;
ra.assign_name("instance name");

// Caller process: invoke the RPC.
auto value = Remotely_accessible::rpc_append("instance name", "Hi", 43);
sintra::console() << value << '\n';
```

### Handle a Remote Exception

```cpp
// Remote exceptions thrown inside append() propagate back across the process boundary.
try {
    sintra::console() << Remotely_accessible::rpc_append("instance", "Hi", 43) << '\n';
}
catch (const std::exception& e) {
    sintra::console() << "Remote RPC failed in callee: " << e.what() << '\n';
}
```

### Observe abnormal exits from managed peers

```cpp
auto crash_monitor = sintra::activate_slot(
    [](const sintra::Managed_process::terminated_abnormally& crash) {
        sintra::console()
            << "Process "
            << sintra::process_of(crash.sender_instance_id)
            << " crashed with status " << crash.status << '\n';
    },
    sintra::Typed_instance_id<sintra::Managed_process>(sintra::any_remote));
```

### Lifeline process ownership

Sintra spawns managed processes with a lifeline pipe/handle. The child watches the
read end; if the parent process exits or unpublishes, the pipe breaks and the child
shuts down, then hard-exits after a timeout.

You can configure the policy per spawn:

```cpp
sintra::Spawn_options options;
options.binary_path = binary_path;
options.lifetime.hard_exit_code = 99;
options.lifetime.hard_exit_timeout_ms = 100;
sintra::spawn_swarm_process(options);
```

Note: spawned processes require a lifeline by default. If you launch a process
manually (outside `spawn_swarm_process`), you must pass lifeline arguments or
disable the check. See `docs/process_lifecycle_notes.md` for details.

### Qt cursor sync example

For a Qt widget example that forwards Qt signals through sintra, see `example/qt_basic/README.md`.

## Advanced topics

### Threading model and barriers

#### Asynchronous message dispatch

Sintra uses **dedicated reader threads** to process incoming messages from shared memory rings. When a message arrives:
1. A reader thread pulls the message from the ring buffer.
2. The reader thread invokes the matching slot or RPC handler **asynchronously**.
3. Handlers (and their post-handler continuations) execute on the reader thread, not the thread that published the message or called the barrier.

**Concurrency reminder:** Slot handlers that touch shared state must still synchronize with other threads in the process (via mutexes, atomics, etc.). The barriers described below coordinate *when* handlers run; they do not eliminate the need for thread-safe data structures.

#### Barrier semantics

`sintra::barrier()` coordinates progress across processes and comes in three flavors that trade off strength for cost. The template defaults to `delivery_fence_t`, so a plain `barrier("name")` is already stronger than a bare rendezvous. The lightest-weight barrier whose guarantees match the code's requirements is preferred:

* **Rendezvous barriers** (`barrier<sintra::rendezvous_t>(name)`) simply ensure that every participant has reached the synchronization point. Messages published before the barrier might still be in flight or waiting to be handled, so this mode is appropriate when only aligned phase progression is needed-for example, coordinating the simultaneous start of a workload whose logic does not depend on the effects of earlier messages.

  *Warning: Two peers can both reach the rendezvous while still missing each other's prior messages (A sends x, B sends y, both call rendezvous, neither is guaranteed to have received the other). Prefer delivery or processing fences when correctness depends on pre-barrier messages being observed.*
* **Delivery-fence barriers** (`barrier(name)` or `barrier<sintra::delivery_fence_t>(name)`) guarantee that all pre-barrier messages have been pulled off the shared-memory rings by each process's reader thread and are queued locally for handling, though the handlers may still be running. The default delivery fence is suitable when the next step requires the complete set of incoming work to be staged, such as inspecting an inbox before taking action.
* **Processing-fence barriers** (`barrier<sintra::processing_fence_t>(name)`) wait until every handler (and any continuations) for messages published before the barrier has finished executing. This mode is appropriate when subsequent logic must observe the completed side effects-for instance, reading shared state that earlier handlers updated or applying a configuration change only after all peers processed preparatory updates.

Delivery fences cost the same as rendezvous plus a short wait for readers to catch up. Processing fences add a single control message per process and an extra rendezvous to allow deterministic observation of handler side effects.

```cpp
// Wait until everyone reaches the same point and any prior messages are queued locally.
sintra::barrier("phase-1"); // delivery fence

// Later: ensure the side effects from earlier messages are visible before reading shared data.
sintra::barrier<sintra::processing_fence_t>("apply-updates");
```

Processing fences are safe to call from any thread, including handlers themselves: reader threads continue draining queued work and post-handlers while the fence waits, so invoking a fence from within a handler keeps the system making progress. When coordination between threads inside the same process is also required, Sintra barriers typically pair with standard threading primitives.

### Optional explicit type ids

Most users do not need explicit type ids as long as every process is built with the same
toolchain and flags. When toolchains are mixed or there is a need to remove any doubt
about type id stability, ids can be pinned explicitly for both transceivers and messages.
The ids must remain unique and consistent across every process in the swarm.

```cpp
struct Explicit_bus : sintra::Derived_transceiver<Explicit_bus>
{
    SINTRA_TYPE_ID(0x120)
    SINTRA_MESSAGE_EXPLICIT(ping, 0x121, int value)
};

Explicit_bus bus;
sintra::activate_slot([](const Explicit_bus::ping& msg) {
    sintra::console() << "ping value=" << msg.value << '\n';
});
bus.emit_global<Explicit_bus::ping>(42);
```

See `example/sintra/sintra_example_7_explicit_type_ids.cpp` for a full example.

## Tests and continuous integration

The library includes a comprehensive test suite covering publish/subscribe, RPC,
barriers, and crash recovery. Tests are controlled by `tests/active_tests.txt`.

```bash
cmake -B build -DSINTRA_BUILD_TESTS=ON
cmake --build build
cd tests && python3 run_tests.py --build-dir ../build --config Release
```

See [TESTING.md](TESTING.md) for detailed documentation.

CI runs on Linux, macOS, Windows (GitHub Actions), and FreeBSD (Cirrus CI).

## License

The source code is licensed under the Simplified BSD License.
