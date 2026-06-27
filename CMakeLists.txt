cmake_minimum_required(VERSION 3.16)

if(CMAKE_HOST_WIN32)
    find_program(SINTRA_POWERSHELL_PATH NAMES pwsh powershell)
    if(NOT SINTRA_POWERSHELL_PATH AND DEFINED ENV{SystemRoot})
        set(_sintra_windows_powershell
            "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
        if(EXISTS "${_sintra_windows_powershell}")
            set(SINTRA_POWERSHELL_PATH "${_sintra_windows_powershell}")
        endif()
    endif()
    if(SINTRA_POWERSHELL_PATH)
        set(Z_VCPKG_POWERSHELL_PATH "${SINTRA_POWERSHELL_PATH}" CACHE INTERNAL
            "PowerShell executable used by vcpkg app-local deployment.")
    endif()
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_sintra_vs_vcpkg_globals
            "VcpkgXUseBuiltInApplocalDeps=true")
        foreach(_sintra_vs_vcpkg_global ${_sintra_vs_vcpkg_globals})
            if(NOT "${_sintra_vs_vcpkg_global}" IN_LIST CMAKE_VS_GLOBALS)
                list(APPEND CMAKE_VS_GLOBALS "${_sintra_vs_vcpkg_global}")
            endif()
        endforeach()
        if(NOT "CMAKE_VS_GLOBALS" IN_LIST CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
            list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_VS_GLOBALS)
        endif()
    endif()
endif()

project(sintra
    VERSION 1.2.0
    LANGUAGES CXX
)

# Default to the newest macOS SDK available on GitHub runners when building on Apple
# platforms, but allow callers to override the deployment target explicitly.
if(APPLE AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum supported macOS version" FORCE)
endif()

find_package(Threads REQUIRED)

option(SINTRA_RELEASE_WITH_DEBUG_SYMBOLS "Emit debug symbols when building Release configuration" ON)
option(SINTRA_ENABLE_COVERAGE "Build with coverage instrumentation" OFF)

if(SINTRA_ENABLE_COVERAGE AND MSVC)
    message(FATAL_ERROR "SINTRA_ENABLE_COVERAGE requires a compiler/toolchain that supports --coverage; MSVC is not supported.")
endif()

# Default to a Debug build when sintra is the top-level project. Avoid touching
# CMAKE_BUILD_TYPE / CMAKE_CONFIGURATION_TYPES when sintra is consumed via
# add_subdirectory or FetchContent: a release-only outer project should not be
# silently flipped to Debug just by pulling sintra in.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
    endif()

    # For MSVC multi-config generators, set debug as default
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
    endif()
endif()

add_library(sintra INTERFACE)
# Provide a namespaced alias so consumers can write `target_link_libraries(app
# PRIVATE sintra::sintra)` and so that a missing dependency fails at configure
# time rather than producing a confusing link error about an unknown target.
# The alias also matches what an installed find_package(sintra) export would
# expose, keeping the in-tree (add_subdirectory / FetchContent) and installed
# usages symmetrical.
add_library(sintra::sintra ALIAS sintra)
# Use BUILD_INTERFACE / INSTALL_INTERFACE so the same target works both when
# consumed from the build tree (in-tree tests, examples, add_subdirectory or
# FetchContent users) and when consumed from an installed prefix via
# find_package(sintra).
target_include_directories(sintra INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(sintra INTERFACE cxx_std_20)
target_link_libraries(sintra INTERFACE Threads::Threads)

# The public sintra target stays flag-neutral: debug/optimization/runtime
# flags must not leak to consumers through the INTERFACE target. Internal
# targets (the examples; the tests manage their own flags in
# tests/CMakeLists.txt) opt in via sintra_dev_flags. CMake's per-config
# defaults already provide the standard debug flags (/Od /Zi /RTC1 /MDd, -g)
# for Debug builds, so only the non-default extras live here.
add_library(sintra_dev_flags INTERFACE)
if(MSVC)
    target_compile_options(sintra_dev_flags INTERFACE
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/Zi>
    )
    target_link_options(sintra_dev_flags INTERFACE
        $<$<CONFIG:Debug>:/DEBUG:FULL>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/DEBUG:FULL>
    )
else()
    target_compile_options(sintra_dev_flags INTERFACE
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:-g>
    )
endif()

option(SINTRA_BUILD_EXAMPLES "Build Sintra examples" ON)
option(SINTRA_BUILD_TESTS "Build Sintra tests" ON)
option(SINTRA_BUILD_MANUAL_TESTS "Build Sintra manual/reproduction tests" OFF)
option(SINTRA_BUILD_QT_EXAMPLES "Build Qt examples (requires Qt6 Widgets)" OFF)

if(SINTRA_BUILD_EXAMPLES)
    add_subdirectory(example/sintra)
    if(SINTRA_BUILD_QT_EXAMPLES)
        add_subdirectory(example/qt_basic)
        add_subdirectory(example/qt_multi_cursor)
    endif()
endif()

if(SINTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Installation & export -------------------------------------------------
# Make sintra discoverable by consumers via find_package(sintra). When sintra
# is consumed as a subproject (add_subdirectory / FetchContent / git submodule)
# we default SINTRA_INSTALL to OFF so the inner project does not unexpectedly
# add install rules to the outer project's install step. The top-level build
# keeps install rules on by default so `cmake --install build` works.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SINTRA_INSTALL_DEFAULT ON)
else()
    set(SINTRA_INSTALL_DEFAULT OFF)
endif()
option(SINTRA_INSTALL "Generate install rules for sintra" ${SINTRA_INSTALL_DEFAULT})

if(SINTRA_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS sintra EXPORT sintraTargets)

    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/sintra
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(EXPORT sintraTargets
        FILE sintraTargets.cmake
        NAMESPACE sintra::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sintra
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sintraConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/sintraConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sintra
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/sintraConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/sintraConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/sintraConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sintra
    )
endif()
