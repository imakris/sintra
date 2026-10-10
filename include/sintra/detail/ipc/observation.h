// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <utility>

namespace sintra::detail {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace test_hooks {
// An outer fixture consumes this only after its ownership and wake obligations
// have completed. Nested observations never erase an earlier failure.
inline thread_local const char* s_deferred_observation_failure = nullptr;

inline const char* take_observation_failure() noexcept
{
    const char* failure = s_deferred_observation_failure;
    s_deferred_observation_failure = nullptr;
    return failure;
}
}
#endif

inline void defer_observation_failure(const char* stage) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (!test_hooks::s_deferred_observation_failure) {
        test_hooks::s_deferred_observation_failure = stage;
    }
#else
    (void)stage;
#endif
}

inline bool observation_failure_pending() noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    return test_hooks::s_deferred_observation_failure != nullptr;
#else
    return false;
#endif
}

template <typename F>
inline void observe_without_canceling(const char* stage, F&& observe) noexcept
{
    try {
        std::forward<F>(observe)();
    }
    catch (...) {
        defer_observation_failure(stage);
    }
}

} // namespace sintra::detail
