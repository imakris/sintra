foreach(required_variable
        SINTRA_BINARY_DIR
        SINTRA_PACKAGE_TEST_SOURCE_DIR
        SINTRA_PACKAGE_TEST_BINARY_DIR
        SINTRA_PACKAGE_VERSION
        SINTRA_PACKAGE_TEST_GENERATOR)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

string(REPLACE "." ";" sintra_version_components "${SINTRA_PACKAGE_VERSION}")
list(LENGTH sintra_version_components sintra_version_component_count)
if(sintra_version_component_count LESS 3)
    message(FATAL_ERROR
        "SINTRA_PACKAGE_VERSION must contain major, minor, and patch components")
endif()

list(GET sintra_version_components 0 sintra_version_major)
list(GET sintra_version_components 1 sintra_version_minor)
list(GET sintra_version_components 2 sintra_version_patch)

if(sintra_version_patch GREATER 0)
    math(EXPR sintra_adjacent_patch "${sintra_version_patch} - 1")
    set(sintra_adjacent_version
        "${sintra_version_major}.${sintra_version_minor}.${sintra_adjacent_patch}")
elseif(sintra_version_minor GREATER 0)
    math(EXPR sintra_adjacent_minor "${sintra_version_minor} - 1")
    set(sintra_adjacent_version
        "${sintra_version_major}.${sintra_adjacent_minor}.0")
else()
    message(FATAL_ERROR
        "The package-version oracle needs a prior same-major version")
endif()

set(sintra_abbreviated_current_version
    "${sintra_version_major}.${sintra_version_minor}")

file(REMOVE_RECURSE "${SINTRA_PACKAGE_TEST_BINARY_DIR}")
set(sintra_install_prefix "${SINTRA_PACKAGE_TEST_BINARY_DIR}/prefix")

set(sintra_install_command
    "${CMAKE_COMMAND}"
    --install "${SINTRA_BINARY_DIR}"
    --prefix "${sintra_install_prefix}")
if(DEFINED SINTRA_PACKAGE_TEST_CONFIG AND
   NOT "${SINTRA_PACKAGE_TEST_CONFIG}" STREQUAL "")
    list(APPEND sintra_install_command
        --config "${SINTRA_PACKAGE_TEST_CONFIG}")
endif()

execute_process(
    COMMAND ${sintra_install_command}
    RESULT_VARIABLE sintra_install_result
    OUTPUT_VARIABLE sintra_install_stdout
    ERROR_VARIABLE sintra_install_stderr
)
if(NOT sintra_install_result EQUAL 0)
    message(FATAL_ERROR
        "Could not install the Sintra package for the version-policy oracle.\n"
        "stdout:\n${sintra_install_stdout}\n"
        "stderr:\n${sintra_install_stderr}")
endif()

function(sintra_configure_consumer case_name requested_version expect_found)
    set(case_binary_dir "${SINTRA_PACKAGE_TEST_BINARY_DIR}/${case_name}")
    set(configure_command
        "${CMAKE_COMMAND}"
        -S "${SINTRA_PACKAGE_TEST_SOURCE_DIR}"
        -B "${case_binary_dir}"
        -G "${SINTRA_PACKAGE_TEST_GENERATOR}"
        "-DSINTRA_PACKAGE_PREFIX=${sintra_install_prefix}"
        "-DSINTRA_REQUESTED_VERSION=${requested_version}"
        "-DSINTRA_EXPECT_FOUND=${expect_found}")

    if(DEFINED SINTRA_PACKAGE_TEST_GENERATOR_PLATFORM AND
       NOT "${SINTRA_PACKAGE_TEST_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND configure_command
            -A "${SINTRA_PACKAGE_TEST_GENERATOR_PLATFORM}")
    endif()
    if(DEFINED SINTRA_PACKAGE_TEST_GENERATOR_TOOLSET AND
       NOT "${SINTRA_PACKAGE_TEST_GENERATOR_TOOLSET}" STREQUAL "")
        list(APPEND configure_command
            -T "${SINTRA_PACKAGE_TEST_GENERATOR_TOOLSET}")
    endif()

    execute_process(
        COMMAND ${configure_command}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_stdout
        ERROR_VARIABLE configure_stderr
    )
    if(NOT configure_result EQUAL 0)
        message(FATAL_ERROR
            "Sintra package consumer '${case_name}' did not configure.\n"
            "stdout:\n${configure_stdout}\n"
            "stderr:\n${configure_stderr}")
    endif()

    if(expect_found)
        set(build_command
            "${CMAKE_COMMAND}" --build "${case_binary_dir}")
        if(DEFINED SINTRA_PACKAGE_TEST_CONFIG AND
           NOT "${SINTRA_PACKAGE_TEST_CONFIG}" STREQUAL "")
            list(APPEND build_command
                --config "${SINTRA_PACKAGE_TEST_CONFIG}")
        endif()

        execute_process(
            COMMAND ${build_command}
            RESULT_VARIABLE build_result
            OUTPUT_VARIABLE build_stdout
            ERROR_VARIABLE build_stderr
        )
        if(NOT build_result EQUAL 0)
            message(FATAL_ERROR
                "Sintra package consumer '${case_name}' did not build.\n"
                "stdout:\n${build_stdout}\n"
                "stderr:\n${build_stderr}")
        endif()
    endif()
endfunction()

sintra_configure_consumer(exact_current "${SINTRA_PACKAGE_VERSION}" TRUE)
sintra_configure_consumer(
    abbreviated_current "${sintra_abbreviated_current_version}" FALSE)
sintra_configure_consumer(adjacent_non_current "${sintra_adjacent_version}" FALSE)
sintra_configure_consumer(versionless "" TRUE)
