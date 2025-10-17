#pragma once

#include <sintra/detail/debug_watchdog.h>

#include <chrono>
#include <cstdlib>

inline void install_test_watchdog(const char* label)
{
#ifndef _WIN32
    const char* raw = std::getenv("SINTRA_TEST_TIMEOUT_SECONDS");
    long timeout = raw ? std::strtol(raw, nullptr, 10) : 30;
    if (timeout > 0) {
        sintra::detail::install_process_watchdog(std::chrono::seconds(timeout), label);
    }
#else
    (void)label;
#endif
}

