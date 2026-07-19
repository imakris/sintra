add_library(sintra_test_config INTERFACE)
target_compile_definitions(sintra_test_config INTERFACE
    "SINTRA_DEBUG_PAUSE_ON_EXIT=(::std::getenv(\"SINTRA_DEBUG_PAUSE_ON_EXIT\") && ::std::getenv(\"SINTRA_DEBUG_PAUSE_ON_EXIT\")[0] != '0')"
    "SINTRA_PRESERVE_SCRATCH=(::std::getenv(\"SINTRA_PRESERVE_SCRATCH\") && ::std::getenv(\"SINTRA_PRESERVE_SCRATCH\")[0] != '0')"
    "SINTRA_TEST_ROOT=::std::getenv(\"SINTRA_TEST_ROOT\")"
    "SINTRA_ENABLE_TEST_HOOKS=1"
)

if(MSVC)
    target_compile_options(sintra_test_config INTERFACE
        /FS
        /bigobj
        $<$<CONFIG:Debug>:/UNDEBUG>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/O1>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/Zi>
        $<$<CONFIG:Release>:/DNDEBUG>
    )
    target_link_options(sintra_test_config INTERFACE
        $<$<CONFIG:Debug>:/DEBUG:FULL>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/DEBUG:FULL>
    )
else()
    target_compile_options(sintra_test_config INTERFACE
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-UNDEBUG>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:-O0>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:-g>
        $<$<CONFIG:Release>:-DNDEBUG>
    )
endif()

if(MINGW)
    target_compile_options(sintra_test_config INTERFACE -Wa,-mbig-obj)
endif()

if(SINTRA_ENABLE_COVERAGE AND NOT MSVC)
    target_compile_options(sintra_test_config INTERFACE $<$<CONFIG:Debug>:--coverage>)
    target_link_options(sintra_test_config INTERFACE $<$<CONFIG:Debug>:--coverage>)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(sintra_test_config INTERFACE atomic)
endif()

target_link_libraries(sintra_test_core PRIVATE sintra_test_config)

add_library(sintra_for_tests INTERFACE)
target_link_libraries(sintra_for_tests INTERFACE
    sintra_test_core
    sintra_test_config
)

file(GLOB TEST_FILES CONFIGURE_DEPENDS "*.cpp")
if(NOT TEST_FILES)
    message(WARNING "No tests found")
    return()
endif()

list(SORT TEST_FILES)

if(SINTRA_BUILD_ACTIVE_TESTS_ONLY)
    set(SINTRA_ACTIVE_TESTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/active_tests.txt")
    if(NOT EXISTS "${SINTRA_ACTIVE_TESTS_FILE}")
        message(FATAL_ERROR "SINTRA_BUILD_ACTIVE_TESTS_ONLY is ON but active_tests.txt was not found")
    endif()

    file(STRINGS "${SINTRA_ACTIVE_TESTS_FILE}" SINTRA_ACTIVE_TEST_LINES)
    set(SINTRA_ACTIVE_TEST_NAMES)
    foreach(SINTRA_ACTIVE_TEST_LINE IN LISTS SINTRA_ACTIVE_TEST_LINES)
        string(REGEX REPLACE "#.*$" "" SINTRA_ACTIVE_TEST_LINE "${SINTRA_ACTIVE_TEST_LINE}")
        string(STRIP "${SINTRA_ACTIVE_TEST_LINE}" SINTRA_ACTIVE_TEST_LINE)
        if(SINTRA_ACTIVE_TEST_LINE STREQUAL "")
            continue()
        endif()

        string(REGEX MATCH "^[^ \t]+" SINTRA_ACTIVE_TEST_NAME "${SINTRA_ACTIVE_TEST_LINE}")
        if(SINTRA_ACTIVE_TEST_NAME MATCHES "^manual/")
            continue()
        endif()

        string(REGEX REPLACE "^sintra_" "" SINTRA_ACTIVE_TEST_NAME "${SINTRA_ACTIVE_TEST_NAME}")
        string(REGEX REPLACE ":.*$" "" SINTRA_ACTIVE_TEST_NAME "${SINTRA_ACTIVE_TEST_NAME}")
        list(APPEND SINTRA_ACTIVE_TEST_NAMES "${SINTRA_ACTIVE_TEST_NAME}")
    endforeach()
    list(REMOVE_DUPLICATES SINTRA_ACTIVE_TEST_NAMES)

    set(SINTRA_FILTERED_TEST_FILES)
    set(SINTRA_MISSING_ACTIVE_TESTS ${SINTRA_ACTIVE_TEST_NAMES})
    foreach(FULL_TEST_PATH IN LISTS TEST_FILES)
        get_filename_component(TEST_NAME "${FULL_TEST_PATH}" NAME_WE)
        if(TEST_NAME IN_LIST SINTRA_ACTIVE_TEST_NAMES)
            list(APPEND SINTRA_FILTERED_TEST_FILES "${FULL_TEST_PATH}")
            list(REMOVE_ITEM SINTRA_MISSING_ACTIVE_TESTS "${TEST_NAME}")
        endif()
    endforeach()

    if(SINTRA_MISSING_ACTIVE_TESTS)
        message(FATAL_ERROR "active_tests.txt lists tests without matching .cpp files: ${SINTRA_MISSING_ACTIVE_TESTS}")
    endif()

    set(TEST_FILES ${SINTRA_FILTERED_TEST_FILES})
endif()

list(LENGTH TEST_FILES TEST_COUNT)
if(SINTRA_BUILD_ACTIVE_TESTS_ONLY)
    message(STATUS "Building ${TEST_COUNT} active tests for CMake configurations")
else()
    message(STATUS "Building ${TEST_COUNT} tests for CMake configurations")
endif()

foreach(FULL_TEST_PATH IN LISTS TEST_FILES)
    get_filename_component(TEST_FILE "${FULL_TEST_PATH}" NAME)
    get_filename_component(TEST_NAME "${FULL_TEST_PATH}" NAME_WE)
    set(TARGET_NAME sintra_${TEST_NAME})

    add_executable(${TARGET_NAME} ${TEST_FILE})
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

if(SINTRA_BUILD_MANUAL_TESTS)
    add_subdirectory(manual)
endif()
