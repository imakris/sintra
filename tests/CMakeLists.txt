# Helper function to create all test executables for a specific configuration
# Parameters:
#   SUFFIX: e.g., "release_adaptive", "debug_hybrid"
#   POLICY_VALUE: 1, 2, or 3
#   IS_DEBUG: TRUE or FALSE
function(add_test_suite SUFFIX POLICY_VALUE IS_DEBUG)
    # Create library target for this configuration
    set(LIB_TARGET sintra_for_${SUFFIX})
    add_library(${LIB_TARGET} INTERFACE)
    target_include_directories(${LIB_TARGET} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(${LIB_TARGET} INTERFACE cxx_std_17)
    target_link_libraries(${LIB_TARGET} INTERFACE Boost::boost Threads::Threads)

    # Set the reading policy
    target_compile_definitions(${LIB_TARGET} INTERFACE SINTRA_RING_READING_POLICY=${POLICY_VALUE})

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

    add_executable(sintra_hanging_test_${SUFFIX} hanging_test.cpp)
endfunction()

# Build 6 complete test suites: 3 policies × 2 build types
add_test_suite("release_adaptive" 3 FALSE)
add_test_suite("release_hybrid" 2 FALSE)
add_test_suite("release_always_spin" 1 FALSE)
add_test_suite("debug_adaptive" 3 TRUE)
add_test_suite("debug_hybrid" 2 TRUE)
add_test_suite("debug_always_spin" 1 TRUE)
