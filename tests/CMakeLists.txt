# Helper function to create all test executables for a specific configuration
# Parameters:
#   SUFFIX: e.g., "release", "debug"
#   IS_DEBUG: TRUE or FALSE
function(add_test_suite SUFFIX IS_DEBUG)
    # Create library target for this configuration
    set(LIB_TARGET sintra_for_${SUFFIX})
    add_library(${LIB_TARGET} INTERFACE)
    target_include_directories(${LIB_TARGET} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(${LIB_TARGET} INTERFACE cxx_std_17)
    target_link_libraries(${LIB_TARGET} INTERFACE Boost::boost Threads::Threads)

    # Link winmm on Windows for timeBeginPeriod/timeEndPeriod
    if(WIN32)
        target_link_libraries(${LIB_TARGET} INTERFACE winmm)
    endif()

    # Set optimization flags
    if(IS_DEBUG)
        if(MSVC)
            target_compile_options(${LIB_TARGET} INTERFACE /Od /Zi /RTC1)
            target_link_options(${LIB_TARGET} INTERFACE /DEBUG:FULL)
        else()
            target_compile_options(${LIB_TARGET} INTERFACE -g -O0)
        endif()
    else()
        if(MSVC)
            target_compile_options(${LIB_TARGET} INTERFACE /O2 /DNDEBUG)
        else()
            target_compile_options(${LIB_TARGET} INTERFACE -O3 -DNDEBUG)
        endif()
    endif()

    # Link OpenMP if available (for timing)
    if(TARGET OpenMP::OpenMP_CXX)
        target_link_libraries(${LIB_TARGET} INTERFACE OpenMP::OpenMP_CXX)
    endif()

    # Create all test executables with this configuration
    add_executable(sintra_dummy_test_${SUFFIX} dummy_test.cpp)

    add_executable(sintra_basic_pubsub_test_${SUFFIX} basic_pub_sub.cpp)
    target_link_libraries(sintra_basic_pubsub_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_ping_pong_test_${SUFFIX} ping_pong_test.cpp)
    target_link_libraries(sintra_ping_pong_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_ping_pong_multi_test_${SUFFIX} ping_pong_multi_test.cpp)
    target_link_libraries(sintra_ping_pong_multi_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_rpc_append_test_${SUFFIX} rpc_append_test.cpp)
    target_link_libraries(sintra_rpc_append_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_recovery_test_${SUFFIX} recovery_test.cpp)
    target_link_libraries(sintra_recovery_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_barrier_flush_test_${SUFFIX} barrier_flush_test.cpp)
    target_link_libraries(sintra_barrier_flush_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_barrier_stress_test_${SUFFIX} barrier_stress_test.cpp)
    target_link_libraries(sintra_barrier_stress_test_${SUFFIX} PRIVATE ${LIB_TARGET})
    if(WIN32)
        target_link_libraries(sintra_barrier_stress_test_${SUFFIX} PRIVATE winmm)
    endif()

    add_executable(sintra_processing_fence_test_${SUFFIX} processing_fence_test.cpp)
    target_link_libraries(sintra_processing_fence_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_variable_buffer_alignment_test_${SUFFIX} variable_buffer_alignment_test.cpp)
    target_link_libraries(sintra_variable_buffer_alignment_test_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_ipc_rings_tests_${SUFFIX} ipc_rings_tests.cpp)
    target_link_libraries(sintra_ipc_rings_tests_${SUFFIX} PRIVATE ${LIB_TARGET})

    add_executable(sintra_spawn_detached_test_${SUFFIX} spawn_detached_test.cpp)
    target_link_libraries(sintra_spawn_detached_test_${SUFFIX} PRIVATE ${LIB_TARGET})

endfunction()

# Build test suites for both build types using the adaptive policy
add_test_suite("release" FALSE)
add_test_suite("debug" TRUE)
