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

#ifndef SINTRA_PENDING_DEFERRAL_H
#define SINTRA_PENDING_DEFERRAL_H

#include "id_types.h"

#include <functional>

namespace sintra::detail {

struct Pending_deferral_state
{
    bool active = false;
    instance_id_type new_fiid = invalid_instance_id;
    std::function<void()> cleanup;
};

inline thread_local Pending_deferral_state s_pending_deferral_state{};

inline void reset_pending_deferral()
{
    s_pending_deferral_state.active = false;
    s_pending_deferral_state.new_fiid = invalid_instance_id;
    s_pending_deferral_state.cleanup = std::function<void()>{};
}

inline void set_pending_deferral(instance_id_type new_fiid, std::function<void()> cleanup)
{
    reset_pending_deferral();
    s_pending_deferral_state.active = true;
    s_pending_deferral_state.new_fiid = new_fiid;
    s_pending_deferral_state.cleanup = std::move(cleanup);
}

inline bool consume_pending_deferral(instance_id_type& new_fiid, std::function<void()>& cleanup)
{
    if (!s_pending_deferral_state.active) {
        return false;
    }

    new_fiid = s_pending_deferral_state.new_fiid;
    cleanup = std::move(s_pending_deferral_state.cleanup);
    reset_pending_deferral();
    return true;
}

} // namespace sintra::detail

#endif // SINTRA_PENDING_DEFERRAL_H
