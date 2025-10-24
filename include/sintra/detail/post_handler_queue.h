// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <deque>
#include <functional>
#include <limits>
#include <utility>

namespace sintra {
namespace detail {

struct Post_handler_task
{
    size_t generation;
    std::function<void()> task;
};

inline thread_local std::deque<Post_handler_task> tl_post_handler_queue;
inline thread_local size_t tl_post_handler_generation_counter = 0;
inline thread_local size_t tl_current_post_handler_generation = 0;

inline size_t current_post_handler_generation()
{
    return tl_current_post_handler_generation;
}

inline void enqueue_post_handler_task(std::function<void()> task)
{
    tl_post_handler_queue.push_back({tl_current_post_handler_generation, std::move(task)});
}

inline void drain_post_handlers_up_to(size_t generation)
{
    while (!tl_post_handler_queue.empty() &&
           tl_post_handler_queue.front().generation <= generation)
    {
        auto entry = std::move(tl_post_handler_queue.front());
        tl_post_handler_queue.pop_front();

        if (!entry.task) {
            continue;
        }

        const auto previous_generation = tl_current_post_handler_generation;
        tl_current_post_handler_generation = entry.generation;
        entry.task();
        tl_current_post_handler_generation = previous_generation;
    }
}

inline void drain_all_post_handlers()
{
    drain_post_handlers_up_to(std::numeric_limits<size_t>::max());
}

inline void drain_post_handlers_before_current_handler()
{
    if (tl_current_post_handler_generation == 0) {
        drain_all_post_handlers();
        return;
    }

    drain_post_handlers_up_to(tl_current_post_handler_generation - 1);
}

class Post_handler_scope
{
public:
    Post_handler_scope()
        : m_previous_generation(tl_current_post_handler_generation)
        , m_generation(++tl_post_handler_generation_counter)
    {
        tl_current_post_handler_generation = m_generation;
    }

    ~Post_handler_scope()
    {
        drain_post_handlers_up_to(m_generation);
        tl_current_post_handler_generation = m_previous_generation;
    }

    Post_handler_scope(const Post_handler_scope&) = delete;
    Post_handler_scope& operator=(const Post_handler_scope&) = delete;

private:
    size_t m_previous_generation;
    size_t m_generation;
};

inline bool has_post_handler_work()
{
    return !tl_post_handler_queue.empty();
}

inline void replace_post_handler_task(std::function<void()> task)
{
    tl_post_handler_queue.clear();
    if (!task) {
        return;
    }

    enqueue_post_handler_task(std::move(task));
}

} // namespace detail
} // namespace sintra

