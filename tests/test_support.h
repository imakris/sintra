#pragma once

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace sintra::tests {

inline bool should_capture_failure_stacks()
{
    const char* value = std::getenv("SINTRA_CAPTURE_FAILURE_STACKS");
    if (!value || !*value) {
        return false;
    }
    std::string_view view(value);
    return !(view == "0" || view == "false" || view == "False" || view == "FALSE");
}

inline std::atomic<bool>& failure_flag()
{
    static std::atomic<bool> flag{false};
    return flag;
}

inline void mark_failure()
{
    failure_flag().store(true, std::memory_order_relaxed);
}

inline int report_exit(int code)
{
    if (code != 0) {
        mark_failure();
    }
    return code;
}

inline void trigger_stack_capture()
{
    static std::atomic<bool> already_triggered{false};
    bool expected = false;
    if (!should_capture_failure_stacks()
        || !failure_flag().load(std::memory_order_relaxed)
        || !already_triggered.compare_exchange_strong(expected, true)) {
        return;
    }

    std::cout.flush();
    std::cerr.flush();
    std::clog.flush();
    std::fflush(nullptr);

#if defined(__unix__) || defined(__APPLE__)
#if defined(RLIMIT_CORE)
    {
        struct rlimit limit {};
        if (getrlimit(RLIMIT_CORE, &limit) == 0) {
            rlim_t desired = limit.rlim_max;
            if (desired == 0 || desired == RLIM_INFINITY) {
                desired = RLIM_INFINITY;
            }
            if (limit.rlim_cur != desired) {
                struct rlimit new_limit = limit;
                new_limit.rlim_cur = desired;
                new_limit.rlim_max = desired;
                setrlimit(RLIMIT_CORE, &new_limit);
            }
        }
    }
#endif
#if defined(__linux__)
    prctl(PR_SET_DUMPABLE, 1);
#endif
#endif

    std::abort();
}

struct Failure_stack_guard {
    ~Failure_stack_guard()
    {
        trigger_stack_capture();
    }
};

inline Failure_stack_guard failure_stack_guard{};

} // namespace sintra::tests
