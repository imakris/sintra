// Shared helpers for choreography-style tests.

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace sintra::test {

inline std::string make_barrier_name(std::string_view prefix, std::uint64_t a)
{
    std::ostringstream oss;
    oss << prefix << '-' << a;
    return oss.str();
}

inline std::string make_barrier_name(std::string_view prefix,
                                     std::uint64_t a,
                                     std::uint64_t b)
{
    std::ostringstream oss;
    oss << prefix << '-' << a << '-' << b;
    return oss.str();
}

inline std::string make_barrier_name(std::string_view prefix,
                                     std::uint64_t a,
                                     std::string_view suffix)
{
    std::ostringstream oss;
    oss << prefix << '-' << a << '-' << suffix;
    return oss.str();
}

inline std::string make_barrier_name(std::string_view prefix,
                                     std::uint64_t a,
                                     std::uint64_t b,
                                     std::string_view suffix)
{
    std::ostringstream oss;
    oss << prefix << '-' << a << '-' << b << '-' << suffix;
    return oss.str();
}

} // namespace sintra::test
