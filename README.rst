Sintra
======

.. image:: https://github.com/imakris/sintra/actions/workflows/main.yml/badge.svg?branch=main&job=Build%20and%20Test%20on%20ubuntu-latest
   :target: https://github.com/imakris/sintra/actions/workflows/main.yml
   :alt: Linux build status

.. image:: https://github.com/imakris/sintra/actions/workflows/main.yml/badge.svg?branch=main&job=Build%20and%20Test%20on%20windows-latest
   :target: https://github.com/imakris/sintra/actions/workflows/main.yml
   :alt: Windows build status

Sintra is a modern C++ library for building type-safe interprocess communication layers.
It provides a common language for sending signals, broadcasting events, and invoking
remote procedures across process boundaries without resorting to fragile string-based
protocols. The library focuses on expressiveness and safety, making it easier to
coordinate modular applications, daemons, and tools that need to communicate reliably.

What makes Sintra useful
------------------------

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

Getting started
---------------

1. Add the ``include/`` directory to your project's include path.
2. Ensure that a C++17 compliant compiler is used (GCC, Clang, or MSVC are supported).
3. Make the following Boost header-only libraries available: ``interprocess``,
   ``type_index``, ``fusion``, ``atomic``, and ``bind``.
4. Explore the ``example/`` directory to see how to set up signal buses, channels, and
   remote call endpoints.

Because everything ships as headers, Sintra works well in monorepos or projects that
prefer vendoring dependencies as git submodules or fetching them during configuration.

Development workflow
--------------------

Continuous integration builds the project on Linux and Windows through the
``main.yml`` GitHub Actions workflow (see the badge above). The workflow configures the
project with CMake, builds it in Release mode, and runs the accompanying tests to ensure
that both the messaging layer and the examples stay healthy.

License
-------

The source code is licensed under the Simplified BSD License.
