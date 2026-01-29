// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <functional>
#include <cassert>

namespace sintra {

// MinGW TLS cleanup has been unstable for non-POD thread_local objects.
// Use a thread_local pointer to a heap-allocated function to avoid TLS
// destructor crashes during thread shutdown.
// The allocation is released explicitly by the request reader thread; if a
// thread exits without calling tl_post_handler_function_release(), the pointer
// is intentionally leaked to avoid TLS destructor crashes.
inline thread_local std::function<void()>* tl_post_handler_function = nullptr;

// Depth counter for post-handler reentrancy. A simple boolean would be cleared
// too early if post-handlers are nested (directly or via run_after_current_handler
// composition). Using a counter ensures the "in post-handler" state remains true
// until all nested levels have exited.
inline thread_local int tl_post_handler_depth = 0;

inline bool tl_in_post_handler() { return tl_post_handler_depth > 0; }

class Post_handler_guard
{
public:
    Post_handler_guard() { ++tl_post_handler_depth; }
    ~Post_handler_guard()
    {
        if (tl_post_handler_depth <= 0) {
            assert(false && "Post_handler_guard depth underflow");
            tl_post_handler_depth = 0;
            return;
        }
        --tl_post_handler_depth;
    }

    Post_handler_guard(const Post_handler_guard&) = delete;
    Post_handler_guard& operator=(const Post_handler_guard&) = delete;
};

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
