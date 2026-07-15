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
        rerun_generation == first_generation)
    {
        return 2;
    }

    return state.mark_retryable(rerun_generation) && state.retryable()
        ? 0
        : 3;
}
