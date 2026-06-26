add_library(sintra_for_tests INTERFACE)
target_link_libraries(sintra_for_tests INTERFACE sintra::sintra)
target_compile_definitions(sintra_for_tests INTERFACE
    "SINTRA_DEBUG_PAUSE_ON_EXIT=(::std::getenv(\"SINTRA_DEBUG_PAUSE_ON_EXIT\") && ::std::getenv(\"SINTRA_DEBUG_PAUSE_ON_EXIT\")[0] != '0')"
    "SINTRA_PRESERVE_SCRATCH=(::std::getenv(\"SINTRA_PRESERVE_SCRATCH\") && ::std::getenv(\"SINTRA_PRESERVE_SCRATCH\")[0] != '0')"
    "SINTRA_TEST_ROOT=::std::getenv(\"SINTRA_TEST_ROOT\")"
    "SINTRA_ENABLE_TEST_HOOKS=1"
)

if(MSVC)
    target_compile_options(sintra_for_tests INTERFACE
        /FS
        /bigobj
        $<$<CONFIG:Debug>:/UNDEBUG>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/O1>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/Zi>
        $<$<CONFIG:Release>:/DNDEBUG>
    )
    target_link_options(sintra_for_tests INTERFACE
        $<$<CONFIG:Debug>:/DEBUG:FULL>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:/DEBUG:FULL>
    )
else()
    target_compile_options(sintra_for_tests INTERFACE
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-UNDEBUG>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:-O0>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${SINTRA_RELEASE_WITH_DEBUG_SYMBOLS}>>:-g>
        $<$<CONFIG:Release>:-DNDEBUG>
    )
endif()

if(SINTRA_ENABLE_COVERAGE AND NOT MSVC)
    target_compile_options(sintra_for_tests INTERFACE $<$<CONFIG:Debug>:--coverage>)
    target_link_options(sintra_for_tests INTERFACE $<$<CONFIG:Debug>:--coverage>)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(sintra_for_tests INTERFACE atomic)
endif()

file(GLOB TEST_FILES CONFIGURE_DEPENDS "*.cpp")
if(NOT TEST_FILES)
    message(WARNING "No tests found")
    return()
endif()

list(SORT TEST_FILES)
list(LENGTH TEST_FILES TEST_COUNT)
message(STATUS "Building ${TEST_COUNT} tests for CMake configurations")

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
