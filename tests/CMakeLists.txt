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

    file(GLOB TEST_FILES CONFIGURE_DEPENDS "*.cpp")

    if(NOT TEST_FILES)
        message(WARNING "No tests found")
        return()
    endif()

    list(SORT TEST_FILES)
    list(LENGTH TEST_FILES TEST_COUNT)
    message(STATUS "Building ${SUFFIX} configuration for ${TEST_COUNT} tests")

    foreach(FULL_TEST_PATH IN LISTS TEST_FILES)
        get_filename_component(TEST_FILE "${FULL_TEST_PATH}" NAME)
        get_filename_component(TEST_NAME "${FULL_TEST_PATH}" NAME_WE)

        add_executable(sintra_${TEST_NAME}_${SUFFIX} ${TEST_FILE})
        target_link_libraries(sintra_${TEST_NAME}_${SUFFIX} PRIVATE ${LIB_TARGET})
        assign_test_suite_configuration(sintra_${TEST_NAME}_${SUFFIX} ${SUITE_CONFIG})

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
