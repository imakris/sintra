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
      <td colspan="2" style="text-align:center;"><a href="https://cirrus-ci.com/github/imakris/sintra"><img alt="FreeBSD Build &amp; Test" src="https://api.cirrus-ci.com/github/imakris/sintra.svg?task=FreeBSD%20Build%20%26%20Test&amp;branch=master" style="display:block;margin:0 auto;"></a></td>
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


## Key features

* **Type-safe APIs across processes** - interfaces are expressed as C++ types, so
  mismatched payloads are detected at compile time instead of surfacing as runtime
  protocol errors.
* **Signal bus and RPC in one package** - publish/subscribe message dispatch and
  synchronous remote procedure calls share the same primitives, allowing you to mix
  patterns as your architecture requires.
* **Header-only distribution** - integrate the library by adding the headers to your
  project; no separate build step or binaries are necessary.
* **Cross-platform design** - shared-memory transport on Linux and Windows.
* **Opt-in crash recovery** - mark critical workers with `sintra::enable_recovery()` so
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
    sintra::console() << Remotely_accessible::rpc_append("instance", "Hi", 43) << '\n';
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

`sintra::barrier()` coordinates progress across processes and comes in three flavors that trade off strength for cost. The template defaults to `delivery_fence_t`, so a plain `barrier("name")` is already stronger than a bare rendezvous. Pick the lightest-weight barrier whose guarantees match what your code must observe:

* **Rendezvous barriers** (`barrier<sintra::rendezvous_t>(name)`) simply ensure that every participant has reached the synchronization point. Messages published before the barrier might still be in flight or waiting to be handled, so use this mode when you only need aligned phase progression-for example, coordinating the simultaneous start of a workload whose logic does not depend on the effects of earlier messages.
* **Delivery-fence barriers** (`barrier(name)` or `barrier<sintra::delivery_fence_t>(name)`) guarantee that all pre-barrier messages have been pulled off the shared-memory rings by each process’s reader thread and are queued locally for handling, though the handlers may still be running. Reach for the default delivery fence when the next step requires the complete set of incoming work to be staged, such as inspecting an inbox before taking action.
* **Processing-fence barriers** (`barrier(sintra::processing_fence_t{}, name)`) wait until every handler (and any continuations) for messages published before the barrier has finished executing. Choose this mode when subsequent logic must observe the completed side effects-for instance, reading shared state that earlier handlers updated or applying a configuration change only after all peers processed preparatory updates.

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
3. Explore the `example/` directory to see how to set up signal buses, channels, and
   remote call endpoints.

Because everything ships as headers, Sintra works well in monorepos or projects that
prefer vendoring dependencies as git submodules or fetching them during configuration.

## Platform requirements

* **macOS** - Sintra always uses `os_sync_wait_on_address` for its interprocess semaphore implementation. The build fails if `<os/os_sync_wait_on_address.h>` or `<os/clock.h>` is missing, so ensure the runner has macOS 15.0 or newer with the Command Line Tools for Xcode 15 or newer installed (the full Xcode IDE is not required). No legacy semaphore fallback is provided or supported.

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
