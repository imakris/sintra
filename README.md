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
      <td colspan="2" style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/build-freebsd.yml"><img alt="FreeBSD Build &amp; Test" src="https://github.com/imakris/sintra/actions/workflows/build-freebsd.yml/badge.svg?branch=master" style="display:block;margin:0 auto;"></a></td>
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
      <td>Linux (gcc, gcov)</td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/coverage.yml"><img alt="Codecov GCC" src="https://codecov.io/gh/imakris/sintra/branch/master/graph/badge.svg?flag=gcc" style="display:block;margin:0 auto;"></a></td>
    </tr>
    <tr>
      <td>Linux (clang, llvm-cov gcov)</td>
      <td style="text-align:center;"><a href="https://github.com/imakris/sintra/actions/workflows/coverage.yml"><img alt="Codecov Clang" src="https://codecov.io/gh/imakris/sintra/branch/master/graph/badge.svg?flag=clang" style="display:block;margin:0 auto;"></a></td>
    </tr>
  </tbody>
</table>


![Header-only](https://img.shields.io/badge/header--only-yes-lightgrey)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-green)

Sintra is a C++20 library for type-safe interprocess communication on a single host.
Independent processes exchange typed messages, broadcast events, and invoke
synchronous or asynchronous RPC through a compile-time-checked API. Named barriers,
crash detection, and opt-in worker respawning provide coordination alongside
communication.

Sintra targets low-latency local IPC, using memory-mapped shared rings and a
coordinator process to connect a swarm. It suits plugin hosts with out-of-process plugins,
GUI front-ends communicating with background services, and test harnesses that
keep multiple workers in sync while exchanging typed data.

## Table of contents

- [Key features](#key-features)
- [Quick example](#quick-example)
- [Getting started](#getting-started)
- [Process and message model](#process-and-message-model)
- [Supported platforms and architectures](#supported-platforms-and-architectures)
- [Reference and guide](#reference-and-guide)
- [Tests and continuous integration](#tests-and-continuous-integration)
- [License](#license)

## Key features

* **Type-safe APIs across processes** - message and RPC interfaces are expressed
  as C++ types, with compile-time payload checks.
* **Publish/subscribe and RPC** - broadcasts, targeted fire-and-forget messages,
  and blocking or asynchronous remote calls share the same runtime.
* **Header-only distribution** - add the headers to your project; a compiled
  core is also available for builds that want to compile it once.
* **No RTTI required** - type ids derive from compile-time signatures, with
  explicit ids available for ABI-compatible builds.
* **Shared-memory transport** - Linux, macOS, Windows, and FreeBSD are supported.
* **Managed process lifetimes** - spawned children have a lifeline to their
  owner; crash recovery is an explicit child opt-in.

## Quick example

This complete program starts a publisher and a receiver from one executable.
The receiver registers its slot before the ready barrier lets the publisher
send. Both branches use a processing fence to finish the handler before
coordinated shutdown.

```cpp
#include <sintra/sintra.h>

struct Market_tick
{
    int    instrument_id;
    double last_price;
};

int sender_branch()
{
    sintra::barrier("market-tick-ready");
    sintra::world() << Market_tick{17, 101.25};
    sintra::barrier<sintra::processing_fence_t>("market-tick-processed");
    return 0;
}

int receiver_branch()
{
    sintra::activate_slot([](const Market_tick& tick) {
        sintra::console()
            << "instrument " << tick.instrument_id
            << " last price " << tick.last_price << '\n';
    });

    sintra::barrier("market-tick-ready");
    sintra::barrier<sintra::processing_fence_t>("market-tick-processed");
    return 0;
}

int main(int argc, char* argv[])
{
    sintra::init(argc, argv, sender_branch, receiver_branch);
    sintra::shutdown();
    return 0;
}
```

The example is also available as
[`sintra_example_9_minimal_pubsub.cpp`](example/sintra/sintra_example_9_minimal_pubsub.cpp).
For RPC, targeted messages, recovery, and Qt integration, follow the
[guide's recipes](docs/guide.md#recipes).

## Getting started

1. Obtain this repository and put its `include/` directory on your project's
   include path, or consume its CMake target as shown below.
2. Use a C++20 compiler: GCC, Clang, and MSVC are supported.
3. Include `<sintra/sintra.h>` for managed processes, messages, RPC, and barriers.
   Include `<sintra/rings.h>` when using the direct ring API.
4. Start with the complete example above or the focused samples in
   [`example/sintra/`](example/sintra/).

### CMake integration

For an in-tree dependency:

```cmake
add_subdirectory(external/sintra)
target_link_libraries(my_app PRIVATE sintra::sintra)
```

Tests and examples default to enabled for a top-level Sintra build and disabled
when included as a subproject. Set `SINTRA_BUILD_TESTS` or
`SINTRA_BUILD_EXAMPLES` explicitly to override these defaults.

Top-level builds export CMake package metadata. After installing Sintra with
`cmake --install`, an installed consumer uses:

```cmake
find_package(sintra CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE sintra::sintra)
```

Header-only use is the default. To compile the non-template message-reader core
once, configure Sintra with `-DSINTRA_BUILD_COMPILED=ON` and link
`sintra::sintra_compiled`. See [build integration](docs/guide.md#cmake-integration)
for separate compilation without CMake, platform libraries, and Windows fault
handling switches.

## Process and message model

### A swarm has a coordinator

`sintra::init(argc, argv, branches...)` initializes the runtime and starts the
declared branches as managed processes. The starter hosts the coordinator,
which manages process membership, instance names, type resolution, and barriers.
The coordinator is trusted infrastructure: if it dies, the swarm dies with it.
Application work hosted in that process shares its fate. See
[initialization and process topology](docs/guide.md#initialization-and-process-topology).

Managed children monitor a lifeline pipe or handle. If their owner disappears
or unpublishes them, they begin teardown and hard-exit after the configured
timeout. A child can call `enable_recovery()` to let the coordinator respawn
its custody after an unexpected exit. This recovery restarts a process; it
does not preserve its application state. Dynamic spawning and admission of
manually launched processes use the
[managed-child and invitation APIs](docs/process_lifecycle_notes.md).

### Dispatch runs on reader threads

Sintra's reader threads invoke slots and transported RPC handlers
asynchronously. Handlers and their post-handler continuations run on those
threads, so shared application state still needs mutexes, atomics, or other
normal C++ synchronization. A barrier coordinates progress between processes;
it does not remove in-process data races.

Use `activate_slot()` for asynchronous delivery and `receive<T>()` for a
blocking receive on a control thread. A handler must not block in `receive()`:
the message it awaits needs the reader thread that it would stop. See
[threading and reentrancy](docs/guide.md#threading-and-reentrancy).

### Message storage has a lifetime

Plain payload structs must satisfy Sintra's
[message payload contract](docs/reference/message_payloads.md). Generated
messages can carry variable-size fields such as `message_string`; those fields
borrow the dispatch frame and must be converted to owning values before the
callback returns if they are to outlive it. `receive_owned<T>()` retains a
complete generated message frame for independent ownership. See
[receiving and owning messages](docs/reference/receive.md).

### Coordinate progress and teardown

A plain `barrier("name")` is a delivery fence: it combines a rendezvous with
local reader progress, while handlers may still be running. Use
`processing_fence_t` from a control thread when the next phase needs completed
handler side effects. The [barrier modes](docs/reference/barrier_modes.md)
describe the guarantees and handler-context exclusions.

Use `shutdown()` when all live participants finish together; each participant
calls it after its branch work. Sintra performs the collective handoff before
tearing down the local runtime. Use `leave()` from a control thread for an
intentional departure while peers continue. The coordinator can leave only
when it is the sole remaining known process. An incomplete managed-child join
retains runtime state for a sequential shutdown retry; see
[shutdown and departure](docs/barriers_and_shutdown.md) for return values,
retry rules, and the optional coordinator hook.

### Participants share an account and compatible builds

Managed swarm participants run under the same operating-system account.
Sintra uses owner-only IPC resources in a trusted temporary location; this
provides no isolation between mutually untrusted processes in that account.
See [IPC resource privacy and cleanup](docs/reference/init.md#ipc-resource-privacy)
for filesystem requirements and cleanup behavior.

Build swarm participants with compatible compiler, standard-library, platform,
architecture, and Sintra ring configurations. Startup rejects an ABI-token
mismatch. A matching token and explicit type ids do not by themselves make raw
C++ object representations interoperable. See
[type identity and compatibility](docs/guide.md#type-ids) and
[ring compatibility](docs/reference/rings.md).

## Supported platforms and architectures

* **Linux, macOS, Windows, FreeBSD** - shared-memory transport is supported on all four.
* **CPU architectures** - Sintra targets x86/x64 and ARM/AArch64. Other
  architectures emit a warning and use a no-op spin pause; performance is not
  guaranteed there.
* **Process visibility** - processes sharing rings must remain mutually visible
  to native process lookup for reader-liveness classification. Linux requires a
  shared PID and time namespace with matching procfs; FreeBSD visibility must
  not be hidden by jail or security policy. See the exact
  [process-visibility requirements](docs/reference/rings.md#process-visibility-requirements).
* **macOS** - requires macOS 15.0 or newer and Command Line Tools for Xcode 15
  or newer. The full Xcode IDE is not required. Missing
  `<os/os_sync_wait_on_address.h>` or `<os/clock.h>` fails the build.

## Reference and guide

- [Guide](docs/guide.md) - concepts, usage patterns, build integration, and recipes.
- [API reference](docs/reference/index.md) - per-symbol contracts and examples.
- [Diagnostics](docs/diagnostics.md) - lookup by compiler error, exception, or runtime symptom.
- [Barriers and shutdown](docs/barriers_and_shutdown.md) and
  [process lifecycle](docs/process_lifecycle_notes.md) - coordination and ownership details.
- [Design principles](docs/design_principles.md) - design constraints for contributors.
- [Deferred work and limitations](docs/deferred_work.md) - maintained development scope.

The [guide](docs/guide.md) also explains how to build and serve the static
reference site locally.

## Tests and continuous integration

The suite covers publish/subscribe, RPC, barriers, and recovery. See
[TESTING.md](TESTING.md) for configuring, building, selecting, and running tests.
CI runs on Linux, macOS, Windows, and FreeBSD through GitHub Actions.

## License

Sintra is licensed under the [Simplified BSD License](LICENSE).
