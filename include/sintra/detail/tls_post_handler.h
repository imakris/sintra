// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <functional>

namespace sintra {

// MinGW TLS cleanup has been unstable for non-POD thread_local objects.
// Use a thread_local pointer to a heap-allocated function to avoid TLS
// destructor crashes during thread shutdown.
// The allocation is released explicitly by the request reader thread; if a
// thread exits without calling tl_post_handler_function_release(), the pointer
// is intentionally leaked to avoid TLS destructor crashes.
inline thread_local std::function<void()>* tl_post_handler_function = nullptr;

inline std::function<void()>& tl_post_handler_function_ref()
{
    if (!tl_post_handler_function) {
        tl_post_handler_function = new std::function<void()>();
    }
    return *tl_post_handler_function;
}

inline bool tl_post_handler_function_ready()
{
    return tl_post_handler_function && static_cast<bool>(*tl_post_handler_function);
}

inline void tl_post_handler_function_clear()
{
    if (tl_post_handler_function) {
        *tl_post_handler_function = {};
    }
}

inline void tl_post_handler_function_release()
{
    if (tl_post_handler_function) {
        delete tl_post_handler_function;
        tl_post_handler_function = nullptr;
    }
}

} // namespace sintra
