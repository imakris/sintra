/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include <chrono>
#include <limits>

#ifdef _OPENMP
# include <omp.h>
#endif

namespace sintra {
namespace detail {

inline double chrono_resolution_seconds() noexcept {
    typedef std::chrono::steady_clock clock;
    typedef clock::period period;
    return static_cast<double>(period::num) / static_cast<double>(period::den);
}

inline double chrono_now_seconds() noexcept {
    typedef std::chrono::steady_clock clock;
    const clock::time_point now = clock::now();
    const std::chrono::duration<double> seconds = now.time_since_epoch();
    return seconds.count();
}

inline double omp_resolution_seconds() noexcept {
#ifdef _OPENMP
    const double tick = omp_get_wtick();
    return (tick > 0.0) ? tick : std::numeric_limits<double>::infinity();
#else
    return std::numeric_limits<double>::infinity();
#endif
}

inline bool should_use_chrono() noexcept {
    const double chrono_res = chrono_resolution_seconds();
    const double omp_res = omp_resolution_seconds();
    if (!(chrono_res > 0.0)) {
        return false;
    }
    return chrono_res < omp_res;
}

} // namespace detail

inline double get_wtime() noexcept {
    static const bool use_chrono = detail::should_use_chrono();
#ifdef _OPENMP
    if (!use_chrono) {
        return omp_get_wtime();
    }
#endif
    return detail::chrono_now_seconds();
}

} // namespace sintra
