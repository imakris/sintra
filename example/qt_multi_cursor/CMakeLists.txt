# Qt Multi-Cursor Example
# This example demonstrates:
# - Multiple windows all sending/receiving cursor positions
# - Crash detection and automatic recovery
# - Normal exit handling

if(NOT SINTRA_BUILD_QT_EXAMPLES)
    return()
endif()

find_package(Qt6 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 Widgets not found; skipping Qt multi-cursor example.")
    return()
endif()

set(CMAKE_AUTOMOC ON)

# Coordinator executable (headless)
add_executable(
    sintra_example_qt_multi_cursor_coordinator
    multi_cursor_coordinator.cpp
    multi_cursor_common.h
)
target_link_libraries(
    sintra_example_qt_multi_cursor_coordinator
    PRIVATE
        sintra
)

# Window executable (Qt GUI)
add_executable(
    sintra_example_qt_multi_cursor_window
    multi_cursor_window.cpp
    multi_cursor_common.h
)
target_link_libraries(
    sintra_example_qt_multi_cursor_window
    PRIVATE
        sintra
        Qt6::Widgets
)
