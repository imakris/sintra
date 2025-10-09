cmake_minimum_required(VERSION 3.16)

project(sintra LANGUAGES CXX)

add_library(sintra INTERFACE)
target_include_directories(sintra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(sintra INTERFACE cxx_std_17)

find_package(OpenMP REQUIRED)
target_link_libraries(sintra INTERFACE OpenMP::OpenMP_CXX)

option(SINTRA_BUILD_TESTS "Build Sintra tests" ON)

if(SINTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
