#pragma once

#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace sintra {

enum class log_level
{
    error,
    warning,
    info,
    debug
};

using log_callback_fn = void(*)(log_level level, const char* message, void* user_data);

namespace detail {

inline void default_log_callback(log_level level, const char* message, void* /*user_data*/)
{
    if (!message) {
        return;
    }
    FILE* stream = (level == log_level::error || level == log_level::warning)
        ? stderr
        : stdout;
    std::fwrite(message, 1, std::strlen(message), stream);
    std::fflush(stream);
}

inline log_callback_fn& log_callback_storage()
{
    static log_callback_fn callback = &default_log_callback;
    return callback;
}

inline void*& log_user_storage()
{
    static void* user_data = nullptr;
    return user_data;
}

inline std::mutex& log_mutex()
{
    static std::mutex mutex;
    return mutex;
}

using sintra::log_level;
using sintra::log_raw;
using sintra::Log_stream;

} // namespace detail

// Set a logging callback; pass nullptr to restore the default sink.
inline void set_log_callback(log_callback_fn callback, void* user_data = nullptr)
{
    if (!callback) {
        callback = &detail::default_log_callback;
        user_data = nullptr;
    }
    std::lock_guard<std::mutex> guard(detail::log_mutex());
    detail::log_callback_storage() = callback;
    detail::log_user_storage() = user_data;
}

inline log_callback_fn get_log_callback(void** user_data = nullptr)
{
    std::lock_guard<std::mutex> guard(detail::log_mutex());
    if (user_data) {
        *user_data = detail::log_user_storage();
    }
    return detail::log_callback_storage();
}

inline void log_raw(log_level level, const char* message)
{
    if (!message) {
        return;
    }
    log_callback_fn callback = nullptr;
    void* user_data = nullptr;
    {
        std::lock_guard<std::mutex> guard(detail::log_mutex());
        callback = detail::log_callback_storage();
        user_data = detail::log_user_storage();
    }
    if (!callback) {
        return;
    }
    try {
        callback(level, message, user_data);
    }
    catch (...) {
    }
}

class Log_stream
{
public:
    explicit Log_stream(log_level level, bool enabled = true, std::string postfix = {})
        : m_level(level)
        , m_enabled(enabled)
        , m_postfix(std::move(postfix))
    {}

    ~Log_stream() noexcept
    {
        if (!m_enabled) {
            return;
        }
        if (!m_postfix.empty()) {
            m_stream << m_postfix;
        }
        const auto text = m_stream.str();
        if (!text.empty()) {
            log_raw(m_level, text.c_str());
        }
    }

    Log_stream(const Log_stream&) = delete;
    Log_stream& operator=(const Log_stream&) = delete;
    Log_stream(Log_stream&& other) noexcept
        : m_stream(std::move(other.m_stream))
        , m_level(other.m_level)
        , m_enabled(other.m_enabled)
        , m_postfix(std::move(other.m_postfix))
    {
        other.m_enabled = false;
        other.m_postfix.clear();
    }
    Log_stream& operator=(Log_stream&& other) noexcept
    {
        if (this != &other) {
            m_stream = std::move(other.m_stream);
            m_level = other.m_level;
            m_enabled = other.m_enabled;
            m_postfix = std::move(other.m_postfix);
            other.m_enabled = false;
            other.m_postfix.clear();
        }
        return *this;
    }

    template <typename T>
    Log_stream& operator<<(const T& value)
    {
        if (m_enabled) {
            m_stream << value;
        }
        return *this;
    }

private:
    std::ostringstream m_stream;
    log_level m_level;
    bool m_enabled{true};
    std::string m_postfix;
};

inline Log_stream ls_info()    { return Log_stream(log_level::info); }
inline Log_stream ls_warning() { return Log_stream(log_level::warning); }
inline Log_stream ls_error()   { return Log_stream(log_level::error); }
inline Log_stream ls_debug()   { return Log_stream(log_level::debug); }

} // namespace sintra

