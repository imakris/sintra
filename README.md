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


![Header-only](https://img.shields.io/badge/header--only-yes-lightgrey)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-green)

Sintra is a C++17 library for type-safe interprocess communication on a single host.
It lets independent processes exchange typed messages, broadcast events, and invoke
RPC-style calls with a compile-time-checked API, avoiding string-based protocols and
external brokers. It also provides coordination primitives like named barriers to
structure multi-process workflows.

## Table of Contents

- [Key Features](#key-features)
- [Getting Started](#getting-started)
- [Quick Example](#quick-example)
- [Platform Support](#platform-support)
- [Core Concepts](#core-concepts)
  - [Broadcasting Messages](#broadcasting-messages)
  - [Receiving Messages](#receiving-messages)
  - [Remote Procedure Calls](#remote-procedure-calls)
  - [Error Handling](#error-handling)
  - [Crash Monitoring](#crash-monitoring)
- [Advanced Topics](#advanced-topics)
  - [Threading Model](#threading-model)
  - [Barrier Semantics](#barrier-semantics)
  - [Explicit Type IDs](#explicit-type-ids)
- [Examples](#examples)
- [Testing](#testing)
- [License](#license)


## Key Features

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


## Getting Started

1. Add the `include/` directory to your project's include path.
2. Use a C++17 compliant compiler (GCC, Clang, or MSVC).
3. Include the Sintra headers and start coding.

Because everything ships as headers, Sintra works well in monorepos or projects that
prefer vendoring dependencies as git submodules or fetching them during configuration.


## Quick Example

```cpp
// Sender process: broadcast a Ping to everyone listening.
sintra::world() << Ping{};

// Receiver process: register a slot to handle incoming Pings.
sintra::activate_slot([](const Ping&) {
    sintra::console() << "Received Ping from another process" << '\n';
});
```


## Platform Support

### Operating Systems

Sintra supports Linux, macOS, Windows, and FreeBSD with shared-memory transport.

**macOS note:** Sintra uses `os_sync_wait_on_address` for its interprocess semaphore
implementation. The build requires macOS 15.0 or newer with the Command Line Tools
for Xcode 15+ installed (the full Xcode IDE is not required).

### CPU Architectures

Sintra targets x86/x64 and ARM/AArch64 CPUs. Other architectures are not supported;
builds emit a warning and use a no-op spin pause (performance not guaranteed).


## Core Concepts

### Broadcasting Messages

Send a message to all listening processes:

```cpp
sintra::world() << Ping{};
```

### Receiving Messages

Register a slot to handle incoming messages asynchronously:

```cpp
sintra::activate_slot([](const Ping&) {
    sintra::console() << "Received Ping from another process" << '\n';
});
```

Or block until a specific message arrives (synchronous receive):

```cpp
// Wait for a Stop signal.
sintra::receive<Stop>();

// Wait for a message and capture its payload.
auto msg = sintra::receive<DataMessage>();
sintra::console() << "value=" << msg.value << '\n';
```

Note: call `receive<T>()` from main/control threads only; do not call it from a message handler.

### Remote Procedure Calls

Export a transceiver method for RPC:

```cpp
struct Remotely_accessible: sintra::Derived_transceiver<Remotely_accessible>
{
    std::string append(const std::string& s, int v) {
        return std::to_string(v) + ": " + s;
    }

    SINTRA_RPC(append); // generates Remotely_accessible::rpc_append(...)
};
```

### Error Handling

Remote exceptions thrown inside RPC methods propagate back across the process boundary:

```cpp
try {
    sintra::console() << Remotely_accessible::rpc_append("instance", "Hi", 43) << '\n';
}
catch (const std::exception& e) {
    sintra::console() << "Remote RPC failed in callee: " << e.what() << '\n';
}
```

### Crash Monitoring

Observe abnormal exits from managed peers:

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


## Advanced Topics

### Threading Model

Sintra uses **dedicated reader threads** to process incoming messages from shared memory rings. When a message arrives:
1. A reader thread pulls the message from the ring buffer.
2. The reader thread invokes the matching slot or RPC handler **asynchronously**.
3. Handlers (and their post-handler continuations) execute on the reader thread, not the thread that published the message or called the barrier.

**Concurrency reminder:** Slot handlers that touch shared state must still synchronize with other threads in the process (via mutexes, atomics, etc.). The barriers described below coordinate *when* handlers run; they do not eliminate the need for thread-safe data structures.

### Barrier Semantics

`sintra::barrier()` coordinates progress across processes and comes in three flavors that trade off strength for cost. The template defaults to `delivery_fence_t`, so a plain `barrier("name")` is already stronger than a bare rendezvous. Use the lightest-weight barrier whose guarantees match your requirements:

* **Rendezvous barriers** (`barrier<sintra::rendezvous_t>(name)`) ensure that every participant has reached the synchronization point. Messages published before the barrier might still be in flight, so this mode is appropriate when only aligned phase progression is needed.
* **Delivery-fence barriers** (`barrier(name)` or `barrier<sintra::delivery_fence_t>(name)`) guarantee that all pre-barrier messages have been pulled off the shared-memory rings and are queued locally for handling, though the handlers may still be running.
* **Processing-fence barriers** (`barrier<sintra::processing_fence_t>(name)`) wait until every handler for messages published before the barrier has finished executing. Use this when subsequent logic must observe completed side effects.

```cpp
// Wait until everyone reaches the same point and any prior messages are queued locally.
sintra::barrier("phase-1"); // delivery fence

// Ensure the side effects from earlier messages are visible before reading shared data.
sintra::barrier<sintra::processing_fence_t>("apply-updates");
```

Processing fences are safe to call from any thread, including handlers themselves.

### Explicit Type IDs

Most users do not need explicit type ids as long as every process is built with the same
toolchain and flags. When toolchains are mixed or there is a need to guarantee type id
stability, ids can be pinned explicitly:

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


## Examples

The `example/` directory contains signal bus, channel, and remote call samples.

For a Qt widget example that forwards Qt signals through sintra, see `example/qt_basic/README.md`.

Typical use cases include:
- Plugin hosts coordinating work with out-of-process plugins
- GUI front-ends communicating with background services
- Distributed test harnesses keeping multiple workers in sync


## Testing

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
