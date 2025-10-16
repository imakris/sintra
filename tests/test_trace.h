#pragma once

#include <sintra/detail/debug_log.h>

#include <string_view>
#include <utility>

namespace sintra::test_trace {

template <typename Writer>
inline void trace(std::string_view scope, Writer&& writer)
{
    sintra::detail::trace_sync(scope, std::forward<Writer>(writer));
}

inline void trace(std::string_view scope)
{
    sintra::detail::trace_sync(scope, [](auto&) {});
}

} // namespace sintra::test_trace

