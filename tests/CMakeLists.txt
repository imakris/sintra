# Function to parse active_tests.txt and return list of test paths
function(read_active_tests OUT_VAR)
    set(ACTIVE_TESTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/active_tests.txt")

    if(NOT EXISTS "${ACTIVE_TESTS_FILE}")
        message(FATAL_ERROR "active_tests.txt not found at ${ACTIVE_TESTS_FILE}")
    endif()

    file(STRINGS "${ACTIVE_TESTS_FILE}" LINES)
    set(TEST_PATHS "")

    foreach(LINE IN LISTS LINES)
        # Strip leading/trailing whitespace
        string(STRIP "${LINE}" LINE)

        # Skip empty lines and comments
        if(LINE STREQUAL "" OR LINE MATCHES "^#")
            continue()
        endif()

        # Parse line: <test_path> <iterations>
        # We only care about test_path for building
        string(REGEX MATCH "^([^ \t]+)" TEST_PATH "${LINE}")
        if(TEST_PATH)
            list(APPEND TEST_PATHS "${TEST_PATH}")
        endif()
    endforeach()

    set(${OUT_VAR} ${TEST_PATHS} PARENT_SCOPE)
endfunction()

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
    target_link_libraries(${LIB_TARGET} INTERFACE Threads::Threads)

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
            if(SINTRA_RELEASE_WITH_DEBUG_SYMBOLS)
                target_compile_options(${LIB_TARGET} INTERFACE /Od /Zi /DNDEBUG)
                target_link_options(${LIB_TARGET} INTERFACE /DEBUG:FULL)
            else()
                target_compile_options(${LIB_TARGET} INTERFACE /O2 /DNDEBUG)
            endif()
        else()
            if(SINTRA_RELEASE_WITH_DEBUG_SYMBOLS)
                target_compile_options(${LIB_TARGET} INTERFACE -Og -g -DNDEBUG)
            else()
                target_compile_options(${LIB_TARGET} INTERFACE -O3 -DNDEBUG)
            endif()
        endif()
    endif()

    # Link OpenMP if available (for timing)
    if(TARGET OpenMP::OpenMP_CXX)
        target_link_libraries(${LIB_TARGET} INTERFACE OpenMP::OpenMP_CXX)
    endif()

    # Read test list from active_tests.txt
    read_active_tests(TEST_PATHS)

    if(NOT TEST_PATHS)
        message(WARNING "No active tests found in active_tests.txt")
        return()
    endif()

    message(STATUS "Building ${SUFFIX} configuration for ${CMAKE_CURRENT_LIST_LENGTH} active tests")

    # Create test executables based on active_tests.txt
    foreach(TEST_PATH IN LISTS TEST_PATHS)
        # Handle paths like "manual/some_test" or just "some_test"
        # Convert path to file location and extract test name
        string(REPLACE "/" ";" PATH_PARTS "${TEST_PATH}")
        list(LENGTH PATH_PARTS PARTS_COUNT)

        if(PARTS_COUNT EQUAL 1)
            # Simple test in main directory: "some_test" -> "some_test.cpp"
            set(TEST_FILE "${TEST_PATH}.cpp")
            set(TEST_NAME "${TEST_PATH}")
            set(SUBDIR "")
        elseif(PARTS_COUNT EQUAL 2)
            # Subdirectory test: "manual/some_test" -> "manual/some_test.cpp"
            list(GET PATH_PARTS 0 SUBDIR)
            list(GET PATH_PARTS 1 TEST_NAME)
            set(TEST_FILE "${SUBDIR}/${TEST_NAME}.cpp")
        else()
            message(WARNING "Unsupported test path format: ${TEST_PATH}")
            continue()
        endif()

        # Check if file exists
        set(FULL_TEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE}")
        if(NOT EXISTS "${FULL_TEST_PATH}")
            message(WARNING "Test file not found: ${FULL_TEST_PATH} (from active_tests.txt entry: ${TEST_PATH})")
            continue()
        endif()

        # Create executable
        add_executable(sintra_${TEST_NAME}_${SUFFIX} ${TEST_FILE})
        target_link_libraries(sintra_${TEST_NAME}_${SUFFIX} PRIVATE ${LIB_TARGET})

        # Include parent directory for manual tests that need test utilities
        if(SUBDIR)
            target_include_directories(sintra_${TEST_NAME}_${SUFFIX} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        endif()

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

# Build test suites for both build types
add_test_suite("release" FALSE)
add_test_suite("debug" TRUE)
