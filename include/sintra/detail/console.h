// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "process/coordinator.h"
#include "globals.h"
#include "logging.h"

#include <cstddef>
#include <exception>
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
        // Best-effort delivery; never propagate. The destructor is implicitly
        // noexcept, so a thrown exception here would call std::terminate.
        // Each layer is wrapped in its own try/catch because Log_stream
        // construction and string materialization can themselves throw
        // (e.g., std::bad_alloc).
        try {
            Coordinator::rpc_print(s_coord_id, m_stream.str());
        }
        catch (const std::exception& e) {
            try {
                Log_stream(log_level::warning)
                    << "sintra::console: rpc_print failed ("
                    << e.what()
                    << "); message dropped\n";
            }
            catch (...) {
                // Logging path itself failed (e.g., bad_alloc inside
                // Log_stream). Swallow so the destructor stays noexcept.
            }
        }
        catch (...) {
            try {
                Log_stream(log_level::warning)
                    << "sintra::console: rpc_print failed (unknown exception); message dropped\n";
            }
            catch (...) {
            }
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
