# Manual tests - reproduction cases and intentionally failing tests.
# These are only built when SINTRA_BUILD_MANUAL_TESTS=ON.

set(MANUAL_TEST_FILES
    barrier_deadlock_reproduction_test.cpp
    crash_capture_child_test.cpp
    crash_capture_self_test.cpp
    guard_pending_writer_integration_test.cpp
)

foreach(TEST_FILE IN LISTS MANUAL_TEST_FILES)
    get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
    set(TARGET_NAME sintra_manual_${TEST_NAME})
    add_executable(${TARGET_NAME} ${TEST_FILE})
    target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(${TARGET_NAME} PRIVATE sintra_for_tests)

    if(APPLE)
        if(NOT DEFINED SINTRA_DSYMUTIL_EXECUTABLE)
            find_program(SINTRA_DSYMUTIL_EXECUTABLE dsymutil)
            if(NOT SINTRA_DSYMUTIL_EXECUTABLE)
                message(FATAL_ERROR "dsymutil is required to generate debug symbols on macOS")
            endif()
        endif()

        add_custom_command(TARGET ${TARGET_NAME}
            POST_BUILD
            COMMAND "${SINTRA_DSYMUTIL_EXECUTABLE}" "$<TARGET_FILE:${TARGET_NAME}>"
            COMMENT "Generating dSYM for $<TARGET_FILE_NAME:${TARGET_NAME}>"
            VERBATIM
        )
    endif()
endforeach()
