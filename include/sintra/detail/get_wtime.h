// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <chrono>

namespace sintra {

// Wall-clock timestamp in seconds with steady_clock for monotonicity.
inline double get_wtime() noexcept
{
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

} // namespace sintra

