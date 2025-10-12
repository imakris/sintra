# Sintra

[![Build - Linux](https://github.com/imakris/sintra/actions/workflows/build-linux.yml/badge.svg)](https://github.com/imakris/sintra/actions/workflows/build-linux.yml)
[![Build - Windows](https://github.com/imakris/sintra/actions/workflows/build-windows.yml/badge.svg)](https://github.com/imakris/sintra/actions/workflows/build-windows.yml)
[![Stress Test - Linux](https://github.com/imakris/sintra/actions/workflows/stress-test-linux.yml/badge.svg)](https://github.com/imakris/sintra/actions/workflows/stress-test-linux.yml)
[![Stress Test - Windows](https://github.com/imakris/sintra/actions/workflows/stress-test-windows.yml/badge.svg)](https://github.com/imakris/sintra/actions/workflows/stress-test-windows.yml) 

![Header-only](https://img.shields.io/badge/header--only-yes-lightgrey)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-green)

Sintra is a C++ library for building type-safe interprocess communication layers.
It provides a common language for sending signals, broadcasting events, and invoking
remote procedures across process boundaries without resorting to fragile string-based
protocols. The library focuses on expressiveness and safety, making it easier to
coordinate modular applications, daemons, and tools that need to communicate reliably.

## Key features

* **Type-safe APIs across processes** – interfaces are expressed as C++ types, so
  mismatched payloads are detected at compile time instead of surfacing as runtime
  protocol errors.
* **Signal bus and RPC in one package** – publish/subscribe message dispatch and
  synchronous remote procedure calls share the same primitives, allowing you to mix
  patterns as your architecture requires.
* **Header-only distribution** – integrate the library by adding the headers to your
  project; no separate build step or binaries are necessary.
* **Cross-platform design** – built on top of Boost.Interprocess and related
  header-only Boost utilities, enabling shared-memory transport on Linux and Windows.

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


## Getting started

1. Add the `include/` directory to your project's include path.
2. Ensure that a C++17 compliant compiler is used (GCC, Clang, or MSVC are supported).
3. Make the following Boost header-only libraries available: `interprocess`,
   `type_index`, `fusion`, `atomic`, and `bind`.
4. Explore the `example/` directory to see how to set up signal buses, channels, and
   remote call endpoints.

Because everything ships as headers, Sintra works well in monorepos or projects that
prefer vendoring dependencies as git submodules or fetching them during configuration.

## Tests and continuous integration

The library ships with a small suite of integration tests that exercise the
publish/subscribe bus, ping-pong channels (single and multi-producer), remote
procedure calls, and the managed-process recovery path. They are defined in the
`tests/` directory and can be executed locally by configuring the project with
`cmake` and running `ctest` from the build tree. For longer stress runs that
mirror the CI configuration, invoke

```
python tests/run_tests.py --repetitions 10 --timeout 30 --kill_stalled_processes --build-dir build --config Release
```

after building; the script repeatedly launches the compiled test binaries and
handles timeouts or stalled processes.

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
