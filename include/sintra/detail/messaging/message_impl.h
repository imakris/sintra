// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

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

    const size_t current_offset = static_cast<size_t>(*variable_buffer::tl_pbytes_to_next_message);
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

    char* data = variable_buffer::tl_message_start_address + aligned_offset;
    assert(
        (reinterpret_cast<std::uintptr_t>(data) % alignof(T)) == 0 &&
        "variable_buffer storage must satisfy alignment requirements."
    );

    std::uninitialized_copy(container.begin(), container.end(), reinterpret_cast<T*>(data));

    const size_t self_offset = static_cast<size_t>(
        reinterpret_cast<char*>(this) - variable_buffer::tl_message_start_address);
    offset_in_bytes = aligned_offset - self_offset;
    *variable_buffer::tl_pbytes_to_next_message = static_cast<uint32_t>(span_end);
}


} // namespae sintra

