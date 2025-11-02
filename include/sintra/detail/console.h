// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "process/coordinator.h"
#include "globals.h"

#include <cstddef>
#include <sstream>

namespace sintra {

class Console
{
public:
    Console() = default;
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    ~Console()
    {
        try {
            Coordinator::rpc_print(s_coord_id, m_stream.str());
        }
        catch (...) {
        }
    }

    template <typename T>
    Console& operator<<(const T& value)
    {
        m_stream << value;
        return *this;
    }

private:
    std::ostringstream m_stream;

    void* operator new(std::size_t);
    void* operator new(std::size_t, void*);
    void* operator new[](std::size_t);
    void* operator new[](std::size_t, void*);
};

using console = Console;

} // namespace sintra
