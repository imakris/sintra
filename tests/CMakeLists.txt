# Create a separate sintra target for tests if OpenMP should be disabled
if(SINTRA_TESTS_DISABLE_OPENMP)
    add_library(sintra_for_tests INTERFACE)
    target_include_directories(sintra_for_tests INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(sintra_for_tests INTERFACE cxx_std_17)
    target_link_libraries(sintra_for_tests INTERFACE Boost::boost)
    # No OpenMP linkage for tests - will use chrono-based timing
    set(SINTRA_TEST_TARGET sintra_for_tests)
else()
    set(SINTRA_TEST_TARGET sintra)
endif()

add_executable(sintra_basic_pubsub_test basic_pub_sub.cpp)
target_link_libraries(sintra_basic_pubsub_test PRIVATE ${SINTRA_TEST_TARGET})

add_test(NAME sintra.basic_pubsub COMMAND sintra_basic_pubsub_test)

add_executable(sintra_ping_pong_test ping_pong_test.cpp)
target_link_libraries(sintra_ping_pong_test PRIVATE ${SINTRA_TEST_TARGET})

add_test(NAME sintra.ping_pong COMMAND sintra_ping_pong_test)

add_executable(sintra_ping_pong_multi_test ping_pong_multi_test.cpp)
target_link_libraries(sintra_ping_pong_multi_test PRIVATE ${SINTRA_TEST_TARGET})

add_test(NAME sintra.ping_pong_multi COMMAND sintra_ping_pong_multi_test)

add_executable(sintra_rpc_append_test rpc_append_test.cpp)
target_link_libraries(sintra_rpc_append_test PRIVATE ${SINTRA_TEST_TARGET})

add_test(NAME sintra.rpc_append COMMAND sintra_rpc_append_test)

add_executable(sintra_recovery_test recovery_test.cpp)
target_link_libraries(sintra_recovery_test PRIVATE ${SINTRA_TEST_TARGET})
add_test(NAME sintra.recovery COMMAND sintra_recovery_test)

add_executable(sintra_barrier_flush_test barrier_flush_test.cpp)
target_link_libraries(sintra_barrier_flush_test PRIVATE ${SINTRA_TEST_TARGET})
add_test(NAME sintra.barrier_flush COMMAND sintra_barrier_flush_test)

add_executable(sintra_barrier_stress_test barrier_stress_test.cpp)
target_link_libraries(sintra_barrier_stress_test PRIVATE ${SINTRA_TEST_TARGET})
add_test(NAME sintra.barrier_stress COMMAND sintra_barrier_stress_test)

add_executable(sintra_variable_buffer_alignment_test variable_buffer_alignment_test.cpp)
target_link_libraries(sintra_variable_buffer_alignment_test PRIVATE ${SINTRA_TEST_TARGET})
add_test(NAME sintra.variable_buffer_alignment COMMAND sintra_variable_buffer_alignment_test)

add_executable(sintra_spawn_detached_test spawn_detached_test.cpp)
target_link_libraries(sintra_spawn_detached_test PRIVATE ${SINTRA_TEST_TARGET})
add_test(NAME sintra.spawn_detached COMMAND sintra_spawn_detached_test)

