#pragma once

#include <sintra/detail/debug_log.h>

#include <string_view>
#include <utility>

// Tests can opt-in to rich synchronization tracing by setting SINTRA_TRACE_SYNC.
// When the volume is overwhelming (e.g. FreeBSD CI), set SINTRA_TRACE_SCOPE to a
// comma-separated allow list such as "managed.*,swarm.*,spawn.*,reader.*,barrier.*,test.*,coordinator.*"
// to restrict the emitted scopes.

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

