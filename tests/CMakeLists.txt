add_executable(sintra_ping_pong_coordinator_crash_test
    ping_pong_coordinator_crash_test.cpp
)

set_target_properties(sintra_ping_pong_coordinator_crash_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

target_link_libraries(sintra_ping_pong_coordinator_crash_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME sintra_ping_pong_coordinator_crash_test COMMAND sintra_ping_pong_coordinator_crash_test)
