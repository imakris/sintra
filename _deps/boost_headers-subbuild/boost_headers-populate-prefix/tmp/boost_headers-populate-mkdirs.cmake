# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/app/_deps/boost_headers-src"
  "/app/_deps/boost_headers-build"
  "/app/_deps/boost_headers-subbuild/boost_headers-populate-prefix"
  "/app/_deps/boost_headers-subbuild/boost_headers-populate-prefix/tmp"
  "/app/_deps/boost_headers-subbuild/boost_headers-populate-prefix/src/boost_headers-populate-stamp"
  "/app/_deps/boost_headers-subbuild/boost_headers-populate-prefix/src"
  "/app/_deps/boost_headers-subbuild/boost_headers-populate-prefix/src/boost_headers-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/app/_deps/boost_headers-subbuild/boost_headers-populate-prefix/src/boost_headers-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/app/_deps/boost_headers-subbuild/boost_headers-populate-prefix/src/boost_headers-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
