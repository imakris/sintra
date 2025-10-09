# Sintra

[![Linux build status](https://github.com/imakris/sintra/actions/workflows/main.yml/badge.svg?branch=master&job=Build%20and%20Test%20on%20ubuntu-latest)](https://github.com/imakris/sintra/actions/workflows/main.yml)
[![Windows build status](https://github.com/imakris/sintra/actions/workflows/main.yml/badge.svg?branch=master&job=Build%20and%20Test%20on%20windows-latest)](https://github.com/imakris/sintra/actions/workflows/main.yml)

![Header-only](https://img.shields.io/badge/header--only-yes-lightgrey)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-green)

Sintra is a modern C++ library for building type-safe interprocess communication layers.
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

## Quick communication snapshots

### Broadcast a Ping and listen from another process

```cpp
// Sender process: announce a shared struct Ping to everyone listening.
sintra::world() << Ping{};

// Receiver process: register a slot so cross-process Pings show up locally.
sintra::activate_slot([](const Ping&) {
    sintra::console() << "Received Ping from another process" << '\n';
});
```

### Expose a transceiver method for RPC

```cpp
struct Remotely_accessible
    : sintra::Derived_transceiver<Remotely_accessible> {
    std::string append(const std::string& s, int v) {
        return std::to_string(v) + ": " + s;
    }

    SINTRA_RPC(append); // generates Remotely_accessible::rpc_append(...)
};
```

### Call the remote method and surface its exceptions

```cpp
// Remote exceptions thrown inside append() propagate back across the process boundary.
try {
    sintra::console() << Remotely_accessible::rpc_append("instance", "Hi", 42) << '\n';
} catch (const std::exception& e) {
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

Only the `Managed_process::terminated_abnormally` signal corresponds to a crash; the
same channel also delivers normal lifecycle updates for managed peers.

## Getting started

1. Add the `include/` directory to your project's include path.
2. Ensure that a C++17 compliant compiler is used (GCC, Clang, or MSVC are supported).
3. Make the following Boost header-only libraries available: `interprocess`,
   `type_index`, `fusion`, `atomic`, and `bind`.
4. Explore the `example/` directory to see how to set up signal buses, channels, and
   remote call endpoints.

Because everything ships as headers, Sintra works well in monorepos or projects that
prefer vendoring dependencies as git submodules or fetching them during configuration.

## License

The source code is licensed under the Simplified BSD License.
