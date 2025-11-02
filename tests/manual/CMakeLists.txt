# Manual tests - reproduction cases and intentionally failing tests
# These are only built when BUILD_MANUAL_TESTS=ON
# They may fail by design and are used for debugging specific issues

function(add_manual_test_suite SUFFIX IS_DEBUG)
    # Reuse the library target from parent tests
    set(LIB_TARGET sintra_for_${SUFFIX})

    set(MANUAL_TEST_FILES
        barrier_deadlock_reproduction_test.cpp
        hang_test.cpp
        coordinator_crash_children_hang_test.cpp
        children_crash_coordinator_deadlock_test.cpp
    )

    foreach(TEST_FILE IN LISTS MANUAL_TEST_FILES)
        get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
        add_executable(sintra_${TEST_NAME}_${SUFFIX} ${TEST_FILE})
        target_link_libraries(sintra_${TEST_NAME}_${SUFFIX} PRIVATE ${LIB_TARGET})

        if(APPLE AND SINTRA_RELEASE_WITH_DEBUG_SYMBOLS AND NOT IS_DEBUG)
            if(NOT DEFINED SINTRA_DSYMUTIL_EXECUTABLE)
                find_program(SINTRA_DSYMUTIL_EXECUTABLE dsymutil)
                if(NOT SINTRA_DSYMUTIL_EXECUTABLE)
                    message(FATAL_ERROR "dsymutil is required to generate debug symbols for Release builds on macOS")
                endif()
            endif()

            add_custom_command(TARGET sintra_${TEST_NAME}_${SUFFIX}
                POST_BUILD
                COMMAND "${SINTRA_DSYMUTIL_EXECUTABLE}" "$<TARGET_FILE:sintra_${TEST_NAME}_${SUFFIX}>"
                COMMENT "Generating dSYM for $<TARGET_FILE_NAME:sintra_${TEST_NAME}_${SUFFIX}>"
                VERBATIM
            )
        endif()
    endforeach()
endfunction()

# Build manual test suites for both build types
add_manual_test_suite("release" FALSE)
add_manual_test_suite("debug" TRUE)
