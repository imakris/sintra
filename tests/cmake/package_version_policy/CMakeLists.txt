cmake_minimum_required(VERSION 3.16)

project(sintra_package_version_policy_consumer LANGUAGES CXX)

foreach(required_variable
        SINTRA_PACKAGE_PREFIX
        SINTRA_REQUESTED_VERSION
        SINTRA_EXPECT_FOUND)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

if(SINTRA_REQUESTED_VERSION STREQUAL "")
    find_package(sintra CONFIG QUIET
        PATHS "${SINTRA_PACKAGE_PREFIX}"
        NO_DEFAULT_PATH)
else()
    find_package(sintra ${SINTRA_REQUESTED_VERSION} CONFIG QUIET
        PATHS "${SINTRA_PACKAGE_PREFIX}"
        NO_DEFAULT_PATH)
endif()

if(SINTRA_EXPECT_FOUND)
    if(NOT sintra_FOUND)
        message(FATAL_ERROR
            "The installed Sintra package did not accept requested version "
            "'${SINTRA_REQUESTED_VERSION}'")
    endif()
    if(NOT TARGET sintra::sintra)
        message(FATAL_ERROR
            "The accepted Sintra package did not import sintra::sintra")
    endif()

    add_executable(sintra_package_consumer main.cpp)
    target_link_libraries(sintra_package_consumer PRIVATE sintra::sintra)
else()
    if(sintra_FOUND)
        message(FATAL_ERROR
            "The installed Sintra package accepted unsupported version request "
            "'${SINTRA_REQUESTED_VERSION}'")
    endif()
    if(TARGET sintra::sintra)
        message(FATAL_ERROR
            "A rejected Sintra package request imported sintra::sintra")
    endif()
endif()
