// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstdint>

#ifdef _WIN32
#    include "../sintra_windows.h"
#else
#    include <unistd.h>
#endif

namespace sintra::detail {

using process_id_type = std::uintmax_t;

inline process_id_type get_current_process_id()
{
#ifdef _WIN32
    return static_cast<process_id_type>(::GetCurrentProcessId());
#else
    return static_cast<process_id_type>(::getpid());
#endif
}

} // namespace sintra::detail

