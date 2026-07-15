// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

int main()
{
    using sintra::detail::Managed_child_release_state;
    using sintra::detail::Release_mode;

    Managed_child_release_state state;
    const auto first_generation = state.request(Release_mode::passive);
    if (!state.mark_failing(first_generation) ||
        state.request(Release_mode::cleanup) != 0 ||
        !state.cleanup_requested() ||
        !state.mark_retryable(first_generation))
    {
        return 1;
    }

    const auto rerun_generation = state.generation();
    if (!state.running(rerun_generation) ||
        rerun_generation == first_generation ||
        !state.cleanup_requested() ||
        state.running(first_generation) ||
        state.mark_retryable(first_generation) ||
        state.mark_released(first_generation))
    {
        return 2;
    }

    if (!state.mark_retryable(rerun_generation) || !state.retryable() ||
        !state.cleanup_requested())
    {
        return 3;
    }

    const auto requested_generation = state.request(Release_mode::passive);
    if (requested_generation == 0 || requested_generation == rerun_generation ||
        !state.running(requested_generation))
    {
        return 4;
    }
    if (state.request(Release_mode::cleanup) != 0 ||
        !state.mark_retryable(requested_generation))
    {
        return 5;
    }

    const auto running_rerun = state.generation();
    if (running_rerun == requested_generation ||
        !state.running(running_rerun) || !state.cleanup_requested())
    {
        return 6;
    }
    return state.mark_retryable(running_rerun) && state.retryable() &&
        state.cleanup_requested()
        ? 0
        : 7;
}
