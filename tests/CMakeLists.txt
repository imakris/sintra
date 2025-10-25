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
    set(TEST_FILES
        barrier_complex_choreography_test.cpp
        barrier_flush_test.cpp
        barrier_pathological_choreography_test.cpp
        barrier_rapid_reuse_test.cpp
        barrier_stress_test.cpp
        basic_pub_sub.cpp
        coordinator_default_groups_test.cpp
        choreography_extreme_test.cpp
        complex_choreography_stress_test.cpp
        complex_choreography_test.cpp
        dummy_test.cpp
        extreme_choreography_test.cpp
        ipc_rings_tests.cpp
        ping_pong_multi_test.cpp
        ping_pong_test.cpp
        processing_fence_test.cpp
        recovery_test.cpp
        rpc_append_test.cpp
        spawn_detached_test.cpp
        variable_buffer_alignment_test.cpp
    )

    foreach(TEST_FILE IN LISTS TEST_FILES)
        get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
        add_executable(sintra_${TEST_NAME}_${SUFFIX} ${TEST_FILE})
        target_link_libraries(sintra_${TEST_NAME}_${SUFFIX} PRIVATE ${LIB_TARGET})
    endforeach()

endfunction()

# Build test suites for both build types using the adaptive policy
add_test_suite("release" FALSE)
add_test_suite("debug" TRUE)
