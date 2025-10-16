#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

inline std::string_view trim_scope(std::string_view scope) noexcept
{
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };

    while (!scope.empty() && is_space(static_cast<unsigned char>(scope.front()))) {
        scope.remove_prefix(1);
    }
    while (!scope.empty() && is_space(static_cast<unsigned char>(scope.back()))) {
        scope.remove_suffix(1);
    }
    return scope;
}

inline const std::vector<std::string>& trace_scope_filters() noexcept
{
    static const std::vector<std::string> filters = [] {
        const char* env = std::getenv("SINTRA_TRACE_SCOPE");
        std::vector<std::string> parsed;
        if (!env || *env == '\0') {
            return parsed;
        }

        std::string_view raw(env);
        size_t pos = 0;
        constexpr std::string_view separators = ",; \t\r\n";
        while (pos <= raw.size()) {
            const size_t next = raw.find_first_of(separators, pos);
            std::string_view token;
            if (next == std::string_view::npos) {
                token = raw.substr(pos);
                pos = raw.size() + 1;
            } else {
                token = raw.substr(pos, next - pos);
                pos = next + 1;
            }

            token = trim_scope(token);
            if (!token.empty()) {
                parsed.emplace_back(token);
            }
        }
        return parsed;
    }();

    return filters;
}

inline bool trace_scope_matches(std::string_view scope) noexcept
{
    const auto& filters = trace_scope_filters();
    if (filters.empty()) {
        return true;
    }

    for (const auto& filter : filters) {
        if (filter == "*") {
            return true;
        }

        if (!filter.empty() && filter.back() == '*') {
            std::string_view prefix(filter.data(), filter.size() - 1);
            if (scope.substr(0, prefix.size()) == prefix) {
                return true;
            }
            continue;
        }

        if (scope == filter) {
            return true;
        }
    }

    return false;
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
    if (!trace_scope_matches(scope)) {
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

