# Create a separate sintra target for tests if OpenMP should be disabled
if(SINTRA_TESTS_DISABLE_OPENMP)
    add_library(sintra_for_tests INTERFACE)
    target_include_directories(sintra_for_tests INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(sintra_for_tests INTERFACE cxx_std_17)
    target_link_libraries(sintra_for_tests INTERFACE Boost::boost Threads::Threads)
    # Link winmm on Windows for timeBeginPeriod/timeEndPeriod (ADAPTIVE_SPIN policy)
    if(WIN32)
        target_link_libraries(sintra_for_tests INTERFACE winmm)
    endif()
    # No OpenMP linkage for tests - will use chrono-based timing
    # ALWAYS compile with debug symbols and disable optimizations
    if(MSVC)
        target_compile_options(sintra_for_tests INTERFACE
            /Zi          # Full debug info
            /Od          # Disable optimizations
            /RTC1        # Runtime checks
            /MDd         # Debug runtime library
        )
        target_link_options(sintra_for_tests INTERFACE /DEBUG:FULL)
    else()
        target_compile_options(sintra_for_tests INTERFACE -g -O0)
    endif()
    set(SINTRA_TEST_TARGET sintra_for_tests)
else()
    set(SINTRA_TEST_TARGET sintra)
endif()

set(SINTRA_TEST_SUPPORT_LIBS)
if(NOT WIN32)
    find_library(EXECINFO_LIB execinfo)
    if(EXECINFO_LIB)
        list(APPEND SINTRA_TEST_SUPPORT_LIBS ${EXECINFO_LIB})
    endif()
endif()

add_executable(sintra_dummy_test dummy_test.cpp)
add_test(NAME sintra.dummy COMMAND sintra_dummy_test)
set_tests_properties(sintra.dummy PROPERTIES FIXTURES_SETUP sintra_ready)
set_tests_properties(sintra.dummy PROPERTIES TIMEOUT 20)

add_executable(sintra_basic_pubsub_test basic_pub_sub.cpp)
target_link_libraries(sintra_basic_pubsub_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})

add_test(NAME sintra.basic_pubsub COMMAND sintra_basic_pubsub_test)
list(APPEND SINTRA_CI_TESTS sintra.basic_pubsub)

add_executable(sintra_ping_pong_test ping_pong_test.cpp)
target_link_libraries(sintra_ping_pong_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})

add_test(NAME sintra.ping_pong COMMAND sintra_ping_pong_test)
list(APPEND SINTRA_CI_TESTS sintra.ping_pong)

add_executable(sintra_ping_pong_multi_test ping_pong_multi_test.cpp)
target_link_libraries(sintra_ping_pong_multi_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})

add_test(NAME sintra.ping_pong_multi COMMAND sintra_ping_pong_multi_test)
list(APPEND SINTRA_CI_TESTS sintra.ping_pong_multi)

add_executable(sintra_rpc_append_test rpc_append_test.cpp)
target_link_libraries(sintra_rpc_append_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})

add_test(NAME sintra.rpc_append COMMAND sintra_rpc_append_test)
list(APPEND SINTRA_CI_TESTS sintra.rpc_append)

add_executable(sintra_recovery_test recovery_test.cpp)
target_link_libraries(sintra_recovery_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})
add_test(NAME sintra.recovery COMMAND sintra_recovery_test)
list(APPEND SINTRA_CI_TESTS sintra.recovery)

add_executable(sintra_barrier_flush_test barrier_flush_test.cpp)
target_link_libraries(sintra_barrier_flush_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})
add_test(NAME sintra.barrier_flush COMMAND sintra_barrier_flush_test)
list(APPEND SINTRA_CI_TESTS sintra.barrier_flush)

add_executable(sintra_barrier_stress_test barrier_stress_test.cpp)
target_link_libraries(sintra_barrier_stress_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})
if(WIN32)
    target_link_libraries(sintra_barrier_stress_test PRIVATE winmm)
endif()
add_test(NAME sintra.barrier_stress COMMAND sintra_barrier_stress_test)
list(APPEND SINTRA_CI_TESTS sintra.barrier_stress)

add_executable(sintra_processing_fence_test processing_fence_test.cpp)
target_link_libraries(sintra_processing_fence_test PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})
add_test(NAME sintra.processing_fence COMMAND sintra_processing_fence_test)
list(APPEND SINTRA_CI_TESTS sintra.processing_fence)

add_executable(sintra_variable_buffer_alignment_test variable_buffer_alignment_test.cpp)
target_link_libraries(sintra_variable_buffer_alignment_test PRIVATE ${SINTRA_TEST_TARGET})
add_test(NAME sintra.variable_buffer_alignment COMMAND sintra_variable_buffer_alignment_test)
list(APPEND SINTRA_CI_TESTS sintra.variable_buffer_alignment)

add_executable(sintra_ipc_rings_tests ipc_rings_tests.cpp)
target_link_libraries(sintra_ipc_rings_tests PRIVATE ${SINTRA_TEST_TARGET} ${SINTRA_TEST_SUPPORT_LIBS})
add_test(NAME sintra.ipc_rings COMMAND sintra_ipc_rings_tests)
list(APPEND SINTRA_CI_TESTS sintra.ipc_rings)

add_executable(sintra_spawn_detached_test spawn_detached_test.cpp)
target_link_libraries(sintra_spawn_detached_test PRIVATE ${SINTRA_TEST_TARGET})
add_test(NAME sintra.spawn_detached COMMAND sintra_spawn_detached_test)
list(APPEND SINTRA_CI_TESTS sintra.spawn_detached)

add_executable(sintra_hanging_test hanging_test.cpp)
add_test(NAME sintra.hanging COMMAND sintra_hanging_test)

if(SINTRA_CI_TESTS)
    set_tests_properties(${SINTRA_CI_TESTS} PROPERTIES FIXTURES_REQUIRED sintra_ready)
    # Guarantee the CI driver sees forward progress even if a test wedges by
    # letting CTest terminate any individual binary after two minutes.
    set_tests_properties(${SINTRA_CI_TESTS} PROPERTIES TIMEOUT 120)
endif()
