cmake_minimum_required(VERSION 3.16)

project(sintra LANGUAGES CXX)

# Default to the newest macOS SDK available on GitHub runners when building on Apple
# platforms, but allow callers to override the deployment target explicitly.
if(APPLE AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum supported macOS version" FORCE)
endif()

find_package(Threads REQUIRED)

option(SINTRA_RELEASE_WITH_DEBUG_SYMBOLS "Emit debug symbols when building Release configuration" ON)

# Force Debug build by default
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# For MSVC multi-config generators, set debug as default
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

add_library(sintra INTERFACE)
target_include_directories(sintra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(sintra INTERFACE cxx_std_17)
target_link_libraries(sintra INTERFACE Threads::Threads)
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
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/Zi>
    )
    target_link_options(sintra INTERFACE
        $<$<CONFIG:Debug>:/DEBUG:FULL>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/DEBUG:FULL>
    )
else()
    target_compile_options(sintra INTERFACE
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O0>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:-g>
    )
endif()

option(SINTRA_BUILD_EXAMPLES "Build Sintra examples" ON)
option(SINTRA_BUILD_TESTS "Build Sintra tests" ON)
option(SINTRA_BUILD_QT_EXAMPLES "Build Qt examples (requires Qt6 Widgets)" OFF)

if(SINTRA_BUILD_EXAMPLES)
    add_subdirectory(example/sintra)
    if(SINTRA_BUILD_QT_EXAMPLES)
        add_subdirectory(example/qt)
    endif()
endif()

if(SINTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
