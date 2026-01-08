# Qt Examples
if(NOT SINTRA_BUILD_QT_EXAMPLES)
    return()
endif()

find_package(Qt6 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 Widgets not found; skipping Qt examples.")
    return()
endif()

set(CMAKE_AUTOMOC ON)

add_executable(
    sintra_example_qt_cursor_sync_sender
    cursor_sync_sender.cpp
    cursor_sync_common.h
)
target_link_libraries(
    sintra_example_qt_cursor_sync_sender
    PRIVATE
        sintra
        Qt6::Widgets
)

add_executable(
    sintra_example_qt_cursor_sync_receiver
    cursor_sync_receiver.cpp
    cursor_sync_common.h
)
target_link_libraries(
    sintra_example_qt_cursor_sync_receiver
    PRIVATE
        sintra
        Qt6::Widgets
)
