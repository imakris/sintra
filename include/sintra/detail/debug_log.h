#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sintra::detail {

inline bool trace_sync_enabled() noexcept
{
    static const bool enabled = [] {
        const char* env = std::getenv("SINTRA_TRACE_SYNC");
        if (!env) {
            return false;
        }
        // Treat empty, "0", and "false" (case-insensitive) as disabled.
        if (*env == '\0') {
            return false;
        }
        std::string_view value(env);
        if (value == "0" || value == "false" || value == "FALSE") {
            return false;
        }
        return true;
    }();
    return enabled;
}

inline unsigned long long current_process_id() noexcept
{
#ifdef _WIN32
    return static_cast<unsigned long long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

inline std::mutex& trace_sync_mutex() noexcept
{
    static std::mutex mtx;
    return mtx;
}

inline void trace_sync_message(const std::string& message)
{
    if (!trace_sync_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(trace_sync_mutex());
    std::cerr << message << std::flush;
}

template <typename Writer>
inline void trace_sync(std::string_view scope, Writer&& writer)
{
    if (!trace_sync_enabled()) {
        return;
    }
    std::ostringstream oss;
    oss << "[sintra.trace] pid=" << current_process_id()
        << " tid=" << std::this_thread::get_id()
        << " scope=" << scope << ' ';
    writer(oss);
    oss << '\n';
    trace_sync_message(oss.str());
}

} // namespace sintra::detail

