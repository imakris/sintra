// test_ring_utils.h
// Shared utilities for ring buffer tests.
// Can be included before or after rings.h - only depends on platform_utils.h

#pragma once

#include "test_utils.h"
#include "sintra/detail/ipc/platform_utils.h"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace sintra::test {

/// RAII wrapper for a temporary ring directory that cleans up on destruction.
struct Temp_ring_dir {
    std::filesystem::path path;

    explicit Temp_ring_dir(const std::string& hint)
    {
        path = unique_scratch_directory(hint);
    }

    ~Temp_ring_dir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    Temp_ring_dir(const Temp_ring_dir&) = delete;
    Temp_ring_dir& operator=(const Temp_ring_dir&) = delete;

    std::string str() const { return path.string(); }
};

/// Calculate a suitable ring element count for tests.
/// The result is guaranteed to be:
///   - A multiple of the system page size
///   - Divisible by 8 (for octile calculations)
///   - At least min_elements
template <typename T>
size_t pick_ring_elements(size_t min_elements = 8)
{
    size_t page_size = sintra::system_page_size();
    size_t ring_bytes = page_size;
    while (true) {
        if (ring_bytes % sizeof(T) == 0) {
            size_t elems = ring_bytes / sizeof(T);
            if (elems % 8 == 0 && elems >= min_elements) {
                return elems;
            }
        }
        ring_bytes += page_size;
        if (ring_bytes > page_size * 1024) {
            throw std::runtime_error("Unable to find suitable ring size");
        }
    }
}

} // namespace sintra::test
