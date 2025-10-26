if(NOT UNIX)
    message(FATAL_ERROR "The ping-pong crash test requires a POSIX platform.")
endif()

add_executable(sintra_ping_pong_crash_test
    ping_pong_coordinator_crash_test.cpp
)

target_compile_features(sintra_ping_pong_crash_test PRIVATE cxx_std_17)

add_test(
    NAME sintra_ping_pong_crash
    COMMAND sintra_ping_pong_crash_test
)
