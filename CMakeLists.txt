cmake_minimum_required(VERSION 3.16)

project(sintra LANGUAGES CXX)

find_package(Threads REQUIRED)

# Force Debug build by default
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# For MSVC multi-config generators, set debug as default
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

# Check if vendored Boost headers exist
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/boost/version.hpp")
    set(SINTRA_USE_VENDORED_BOOST_DEFAULT ON)
else()
    set(SINTRA_USE_VENDORED_BOOST_DEFAULT OFF)
endif()

option(SINTRA_USE_VENDORED_BOOST "Use vendored Boost headers from third_party/boost" ${SINTRA_USE_VENDORED_BOOST_DEFAULT})

if(SINTRA_USE_VENDORED_BOOST)
    message(STATUS "Using vendored Boost headers from third_party/boost")
    set(BOOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost")
else()
    message(STATUS "Fetching Boost headers via FetchContent")
    include(FetchContent)

    set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../_deps")
    file(MAKE_DIRECTORY "${FETCHCONTENT_BASE_DIR}")

    FetchContent_Declare(
        boost_headers
        URL
            https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.gz
            https://boostorg.jfrog.io/artifactory/main/release/1.89.0/source/boost_1_89_0.tar.gz
        URL_HASH SHA256=b5a3c209b1c99ff8b5ea70430b6f2a4347ecca0df4f7bba28b3969cc4f895a46
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(boost_headers)
    FetchContent_GetProperties(boost_headers)
    set(BOOST_INCLUDE_DIR "${boost_headers_SOURCE_DIR}")
endif()

if(NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED)
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${BOOST_INCLUDE_DIR}"
    )
endif()

add_library(sintra INTERFACE)
target_include_directories(sintra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(sintra INTERFACE cxx_std_17)
target_link_libraries(sintra INTERFACE Boost::boost Threads::Threads)
# Link winmm on Windows for timeBeginPeriod/timeEndPeriod (ADAPTIVE_SPIN policy)
if(WIN32)
    target_link_libraries(sintra INTERFACE winmm)
endif()

# Apply debug-friendly flags only when building Debug configurations
if(MSVC)
    target_compile_options(sintra INTERFACE
        $<$<CONFIG:Debug>:/Zi>          # Full debug info
        $<$<CONFIG:Debug>:/Od>          # Disable optimizations
        $<$<CONFIG:Debug>:/RTC1>        # Runtime checks
        $<$<CONFIG:Debug>:/MDd>         # Debug runtime library
    )
    target_link_options(sintra INTERFACE $<$<CONFIG:Debug>:/DEBUG:FULL>)
else()
    target_compile_options(sintra INTERFACE
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O0>
    )
endif()

# OpenMP support - optional for timing
option(SINTRA_DISABLE_OPENMP "Disable OpenMP dependency (uses chrono for timing)" OFF)

if(NOT SINTRA_DISABLE_OPENMP)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        find_package(OpenMP)
        if(OpenMP_FOUND)
            target_link_libraries(sintra INTERFACE OpenMP::OpenMP_CXX)
        endif()
    else()
        find_package(OpenMP REQUIRED)
        target_link_libraries(sintra INTERFACE OpenMP::OpenMP_CXX)
    endif()
endif()

option(SINTRA_BUILD_EXAMPLES "Build Sintra examples" ON)
option(SINTRA_BUILD_TESTS "Build Sintra tests" ON)
option(SINTRA_TESTS_DISABLE_OPENMP "Disable OpenMP in tests (uses chrono for timing)" ON)

if(SINTRA_BUILD_EXAMPLES)
    add_subdirectory(example/sintra)
endif()

if(SINTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
