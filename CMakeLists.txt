cmake_minimum_required(VERSION 3.16)

project(sintra LANGUAGES CXX)

include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../_deps")
file(MAKE_DIRECTORY "${FETCHCONTENT_BASE_DIR}")

FetchContent_Declare(
    boost_headers
    URL
        https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz
        https://boostorg.jfrog.io/artifactory/main/release/1.84.0/source/boost_1_84_0.tar.gz
    URL_HASH SHA256=a5800f405508f5df8114558ca9855d2640a2de8f0445f051fa1c7c3383045724
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(boost_headers)
FetchContent_GetProperties(boost_headers)

if(NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED)
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${boost_headers_SOURCE_DIR}"
    )
endif()

add_library(sintra INTERFACE)
target_include_directories(sintra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(sintra INTERFACE cxx_std_17)
target_link_libraries(sintra INTERFACE Boost::boost)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_package(OpenMP)
    if(OpenMP_FOUND)
        target_link_libraries(sintra INTERFACE OpenMP::OpenMP_CXX)
    endif()
else()
    find_package(OpenMP REQUIRED)
    target_link_libraries(sintra INTERFACE OpenMP::OpenMP_CXX)
endif()

option(SINTRA_BUILD_TESTS "Build Sintra tests" ON)

if(SINTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
