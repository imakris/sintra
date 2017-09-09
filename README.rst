Sintra
======

A C++ library for type safe Interprocess signal dispatch and remote procedure calls.

Overview
--------

The library was written to provide the message dispatch mechanism for VIK, an OpenGL based GUI framework, which is currently not open source.

Sintra is a header-only library.
It has dependencies on header-only boost libraries (interprocess, type_index, fusion, atomic, bind).

It has been tested on Windows with Visual Studio 2015 and Linux with gcc 6.3.
However, it has not been tested very thoroughly on Linux.

Usage
-----

Please see under the ``example/`` directory for some usage examples.

License
-------

The source code is licensed under the Simplified BSD License.
