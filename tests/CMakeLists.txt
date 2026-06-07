# Function to parse active_tests.txt and return list of test paths
function(read_active_tests OUT_VAR)
    set(ACTIVE_TESTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/active_tests.txt")

    if(NOT EXISTS "${ACTIVE_TESTS_FILE}")
        message(FATAL_ERROR "active_tests.txt not found at ${ACTIVE_TESTS_FILE}")
    endif()

    file(STRINGS "${ACTIVE_TESTS_FILE}" LINES)
    set(TEST_PATHS "")
    set(LINE_NUMBER 0)

    foreach(LINE IN LISTS LINES)
        math(EXPR LINE_NUMBER "${LINE_NUMBER} + 1")

        # Strip leading/trailing whitespace
        string(STRIP "${LINE}" LINE)

        # Skip empty lines and comments
        if(LINE STREQUAL "" OR LINE MATCHES "^#")
            continue()
        endif()

        if(NOT LINE MATCHES "^([^ \t]+)[ \t]+([1-9][0-9]*)$")
            message(FATAL_ERROR
                "Malformed active_tests.txt entry at line ${LINE_NUMBER}: '${LINE}'. "
                "Expected '<test_path> <iterations>' with iterations >= 1.")
        endif()

        list(APPEND TEST_PATHS "${CMAKE_MATCH_1}")
    endforeach()

    set(${OUT_VAR} ${TEST_PATHS} PARENT_SCOPE)
endfunction()

function(assign_test_suite_configuration TARGET_NAME SUITE_CONFIG)
    if(NOT CMAKE_CONFIGURATION_TYPES)
        return()
    endif()

    string(TOUPPER "${SUITE_CONFIG}" SUITE_CONFIG_UPPER)

    if(CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
        set_property(TARGET ${TARGET_NAME} PROPERTY
            EXCLUDE_FROM_ALL "$<NOT:$<CONFIG:${SUITE_CONFIG}>>")
    endif()

    foreach(CONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${CONFIG}" CONFIG_UPPER)

        if(CONFIG_UPPER STREQUAL SUITE_CONFIG_UPPER)
            set(EXCLUDE_FROM_DEFAULT_BUILD FALSE)
        else()
            set(EXCLUDE_FROM_DEFAULT_BUILD TRUE)
        endif()

        set_property(TARGET ${TARGET_NAME} PROPERTY
            EXCLUDE_FROM_DEFAULT_BUILD_${CONFIG_UPPER} ${EXCLUDE_FROM_DEFAULT_BUILD})
    endforeach()
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
    target_compile_features(${LIB_TARGET} INTERFACE cxx_std_20)
    target_compile_definitions(${LIB_TARGET} INTERFACE
        "SINTRA_DEBUG_PAUSE_ON_EXIT=(::std::getenv(\"SINTRA_DEBUG_PAUSE_ON_EXIT\") && ::std::getenv(\"SINTRA_DEBUG_PAUSE_ON_EXIT\")[0] != '0')"
        "SINTRA_PRESERVE_SCRATCH=(::std::getenv(\"SINTRA_PRESERVE_SCRATCH\") && ::std::getenv(\"SINTRA_PRESERVE_SCRATCH\")[0] != '0')"
        "SINTRA_TEST_ROOT=::std::getenv(\"SINTRA_TEST_ROOT\")"
        "SINTRA_ENABLE_TEST_HOOKS=1"
    )
    target_link_libraries(${LIB_TARGET} INTERFACE Threads::Threads)

    # Link winmm on Windows for timeBeginPeriod/timeEndPeriod
    if(WIN32)
        target_link_libraries(${LIB_TARGET} INTERFACE winmm)
    endif()
    if(MSVC)
        target_compile_options(${LIB_TARGET} INTERFACE /FS)
    endif()

    # Clang on Linux needs libatomic for some atomic operations
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${LIB_TARGET} INTERFACE atomic)
    endif()

    # Coverage instrumentation (only for debug builds on non-MSVC)
    if(SINTRA_ENABLE_COVERAGE AND IS_DEBUG AND NOT MSVC)
        target_compile_options(${LIB_TARGET} INTERFACE --coverage)
        target_link_options(${LIB_TARGET} INTERFACE --coverage)
    endif()

    if(IS_DEBUG)
        set(SUITE_CONFIG Debug)
    else()
        set(SUITE_CONFIG Release)
    endif()

    assign_test_suite_configuration(${LIB_TARGET} ${SUITE_CONFIG})

    # Set optimization flags based on the suite, independent of CMAKE_BUILD_TYPE.
    if(IS_DEBUG)
        if(MSVC)
            if(CMAKE_CONFIGURATION_TYPES)
                target_compile_options(${LIB_TARGET} INTERFACE
                    $<$<CONFIG:${SUITE_CONFIG}>:/Od>
                    $<$<CONFIG:${SUITE_CONFIG}>:/Zi>
                    $<$<CONFIG:${SUITE_CONFIG}>:/RTC1>
                    $<$<CONFIG:${SUITE_CONFIG}>:/UNDEBUG>
                )
                target_link_options(${LIB_TARGET} INTERFACE
                    $<$<CONFIG:${SUITE_CONFIG}>:/DEBUG:FULL>)
            else()
                target_compile_options(${LIB_TARGET} INTERFACE
                    /Od
                    /Zi
                    /RTC1
                    /UNDEBUG
                )
                target_link_options(${LIB_TARGET} INTERFACE /DEBUG:FULL)
            endif()
        else()
            target_compile_options(${LIB_TARGET} INTERFACE
                -g
                -O0
                -UNDEBUG
            )
        endif()
    else()
        if(MSVC)
            if(SINTRA_RELEASE_WITH_DEBUG_SYMBOLS)
                if(CMAKE_CONFIGURATION_TYPES)
                    target_compile_options(${LIB_TARGET} INTERFACE
                        $<$<CONFIG:${SUITE_CONFIG}>:/O1>
                        $<$<CONFIG:${SUITE_CONFIG}>:/Zi>
                        $<$<CONFIG:${SUITE_CONFIG}>:/DNDEBUG>
                    )
                    target_link_options(${LIB_TARGET} INTERFACE
                        $<$<CONFIG:${SUITE_CONFIG}>:/DEBUG:FULL>)
                else()
                    target_compile_options(${LIB_TARGET} INTERFACE
                        /O1
                        /Zi
                        /DNDEBUG
                    )
                    target_link_options(${LIB_TARGET} INTERFACE /DEBUG:FULL)
                endif()
            else()
                if(CMAKE_CONFIGURATION_TYPES)
                    target_compile_options(${LIB_TARGET} INTERFACE
                        $<$<CONFIG:${SUITE_CONFIG}>:/O2>
                        $<$<CONFIG:${SUITE_CONFIG}>:/DNDEBUG>
                    )
                else()
                    target_compile_options(${LIB_TARGET} INTERFACE
                        /O2
                        /DNDEBUG
                    )
                endif()
            endif()
        else()
            if(SINTRA_RELEASE_WITH_DEBUG_SYMBOLS)
                target_compile_options(${LIB_TARGET} INTERFACE
                    -O0
                    -g
                    -DNDEBUG
                )
            else()
                target_compile_options(${LIB_TARGET} INTERFACE
                    -O3
                    -DNDEBUG
                )
            endif()
        endif()
    endif()

    # Read test list from active_tests.txt
    read_active_tests(TEST_PATHS)

    if(NOT TEST_PATHS)
        message(WARNING "No active tests found in active_tests.txt")
        return()
    endif()

    list(LENGTH TEST_PATHS ACTIVE_TEST_COUNT)
    message(STATUS "Building ${SUFFIX} configuration for ${ACTIVE_TEST_COUNT} active tests")

    # Create test executables based on active_tests.txt
    foreach(TEST_PATH IN LISTS TEST_PATHS)
        # Handle paths like "manual/some_test" or just "some_test"
        # Convert path to file location and extract test name
        string(REPLACE "/" ";" PATH_PARTS "${TEST_PATH}")
        list(LENGTH PATH_PARTS PARTS_COUNT)

        if(PARTS_COUNT EQUAL 1)
            # Simple test in main directory: "some_test" -> "some_test.cpp"
            set(TEST_NAME "${TEST_PATH}")
            set(SUBDIR "")
        elseif(PARTS_COUNT EQUAL 2)
            # Subdirectory test: "manual/some_test" -> "manual/some_test.cpp"
            list(GET PATH_PARTS 0 SUBDIR)
            list(GET PATH_PARTS 1 TEST_NAME)
        else()
            message(WARNING "Unsupported test path format: ${TEST_PATH}")
            continue()
        endif()

        set(CONFIG_FILTER "")
        if(TEST_NAME MATCHES "^(.*)_debug$")
            set(TEST_NAME "${CMAKE_MATCH_1}")
            set(CONFIG_FILTER "debug")
        elseif(TEST_NAME MATCHES "^(.*)_release$")
            set(TEST_NAME "${CMAKE_MATCH_1}")
            set(CONFIG_FILTER "release")
        endif()

        if(CONFIG_FILTER AND NOT CONFIG_FILTER STREQUAL SUFFIX)
            continue()
        endif()

        if(SUBDIR)
            set(TEST_FILE "${SUBDIR}/${TEST_NAME}.cpp")
        else()
            set(TEST_FILE "${TEST_NAME}.cpp")
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
        assign_test_suite_configuration(sintra_${TEST_NAME}_${SUFFIX} ${SUITE_CONFIG})

        # Include parent directory for manual tests that need test utilities
        if(SUBDIR)
            target_include_directories(sintra_${TEST_NAME}_${SUFFIX} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        endif()

        if(APPLE)
            if(NOT DEFINED SINTRA_DSYMUTIL_EXECUTABLE)
                find_program(SINTRA_DSYMUTIL_EXECUTABLE dsymutil)
                if(NOT SINTRA_DSYMUTIL_EXECUTABLE)
                    message(FATAL_ERROR "dsymutil is required to generate debug symbols on macOS")
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

if(SINTRA_BUILD_MANUAL_TESTS)
    add_subdirectory(manual)
endif()
