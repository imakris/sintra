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

#ifndef SINTRA_MESSAGE_IMPL_H
#define SINTRA_MESSAGE_IMPL_H


#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>


namespace sintra {


template <typename TC, typename T>
variable_buffer::variable_buffer(const TC& container)
{
    static_assert(
        std::is_trivially_copyable_v<T>,
        "variable_buffer requires trivially copyable element types."
    );

    const size_t element_count = container.size();

    if (element_count > (std::numeric_limits<size_t>::max() / sizeof(T))) {
        throw std::runtime_error("sintra::variable_buffer overflow: container too large");
    }

    num_bytes = element_count * sizeof(T);

    if (num_bytes > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("sintra::variable_buffer overflow: payload exceeds 32-bit limit");
    }

    const size_t current_offset = static_cast<size_t>(*S::tl_pbytes_to_next_message);
    const size_t aligned_offset = detail::align_up_size(current_offset, alignof(T));

    if (aligned_offset > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("sintra::variable_buffer overflow: aligned offset exceeds representable range");
    }

    if (num_bytes > (std::numeric_limits<size_t>::max() - aligned_offset)) {
        throw std::runtime_error("sintra::variable_buffer overflow: message span exceeds representable range");
    }

    const size_t span_end = aligned_offset + num_bytes;
    if (span_end > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("sintra::variable_buffer overflow: message span exceeds representable range");
    }

    char* data = S::tl_message_start_address + aligned_offset;
    assert(
        (reinterpret_cast<std::uintptr_t>(data) % alignof(T)) == 0 &&
        "variable_buffer storage must satisfy alignment requirements."
    );

    std::uninitialized_copy(container.begin(), container.end(), reinterpret_cast<T*>(data));

    const size_t self_offset = static_cast<size_t>(
        reinterpret_cast<char*>(this) - S::tl_message_start_address);
    offset_in_bytes = aligned_offset - self_offset;
    *S::tl_pbytes_to_next_message = static_cast<uint32_t>(span_end);
}


} // namespae sintra

#endif
