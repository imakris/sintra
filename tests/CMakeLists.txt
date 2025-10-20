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

endfunction()

# Select which test matrix to build. Quick builds keep only a single Release
# configuration so developers can iterate rapidly, whereas CI enables the
# entire matrix (Release + Debug × policy variants) for soak coverage.
set(SINTRA_TEST_MATRIX "quick" CACHE STRING "Choose the test matrix to build: quick, release, or full")
set_property(CACHE SINTRA_TEST_MATRIX PROPERTY STRINGS quick release full)

set(SINTRA_QUICK_TEST_SUITES
    release_adaptive 3 FALSE
)

set(SINTRA_RELEASE_TEST_SUITES
    release_adaptive 3 FALSE
    release_hybrid 2 FALSE
    release_always_spin 1 FALSE
)

set(SINTRA_FULL_TEST_SUITES
    ${SINTRA_RELEASE_TEST_SUITES}
    debug_adaptive 3 TRUE
    debug_hybrid 2 TRUE
    debug_always_spin 1 TRUE
)

if(SINTRA_TEST_MATRIX STREQUAL "quick")
    set(_sintra_selected_suites ${SINTRA_QUICK_TEST_SUITES})
elseif(SINTRA_TEST_MATRIX STREQUAL "release")
    set(_sintra_selected_suites ${SINTRA_RELEASE_TEST_SUITES})
elseif(SINTRA_TEST_MATRIX STREQUAL "full")
    set(_sintra_selected_suites ${SINTRA_FULL_TEST_SUITES})
else()
    message(FATAL_ERROR "Unknown SINTRA_TEST_MATRIX selection: ${SINTRA_TEST_MATRIX}")
endif()

message(STATUS "Configuring Sintra tests with '${SINTRA_TEST_MATRIX}' matrix")

list(LENGTH _sintra_selected_suites _sintra_suite_length)
if(NOT _sintra_suite_length)
    message(FATAL_ERROR "No test suites selected for SINTRA_TEST_MATRIX=${SINTRA_TEST_MATRIX}")
endif()

math(EXPR _sintra_mod "${_sintra_suite_length} % 3")
if(NOT _sintra_mod EQUAL 0)
    message(FATAL_ERROR "Internal error: expected suite descriptors to have triplets, got length ${_sintra_suite_length}")
endif()

math(EXPR _sintra_suite_count "${_sintra_suite_length} / 3")
math(EXPR _sintra_last_idx "${_sintra_suite_count} - 1")
foreach(_idx RANGE 0 ${_sintra_last_idx})
    math(EXPR _base "${_idx} * 3")
    math(EXPR _policy_index "${_base} + 1")
    math(EXPR _debug_index "${_base} + 2")
    list(GET _sintra_selected_suites ${_base} _suite_suffix)
    list(GET _sintra_selected_suites ${_policy_index} _suite_policy)
    list(GET _sintra_selected_suites ${_debug_index} _suite_is_debug)
    add_test_suite("${_suite_suffix}" "${_suite_policy}" "${_suite_is_debug}")
endforeach()
