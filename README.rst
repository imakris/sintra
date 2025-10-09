Sintra
======

A C++ library for type safe Interprocess signal dispatch and remote procedure calls.

Overview
--------

The library was initially written to provide a message dispatch mechanism for VIK, an OpenGL based GUI framework, which is currently not open source. This version of the library is repurposed for more generic usage and it is currently under development.

Sintra is a header-only library.
It has dependencies on header-only boost libraries (interprocess, type_index, fusion, atomic, bind).

Testing
-------

The integration tests under ``tests/`` use Boost.Interprocess primitives that
are distributed with the main Boost development packages.  Ensure the
following system dependencies are installed before configuring the project:

* ``libboost-all-dev`` (Debian/Ubuntu) or the equivalent Boost development
  headers on your platform.

Once the dependencies are available, the new pub/sub regression test from the
latest commit can be built and executed with::

   cmake -S . -B build
   cmake --build build
   ctest --test-dir build

It will need a C++17 compiler.

Usage
-----

Please see under the ``example/`` directory for some usage examples.

License
-------

The source code is licensed under the Simplified BSD License.
