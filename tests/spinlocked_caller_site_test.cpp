// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// Build both variants header-only: mixing feature selections with the compiled
// Sintra core would give different inline definitions in one executable.
#include <version>
#if __has_include(<source_location>)
#include <source_location>
#endif
#ifdef SINTRA_TEST_DISABLE_SOURCE_LOCATION
#undef __cpp_lib_source_location
#endif

#include <sintra/sintra.h>
#include "support/native_stderr_capture.h"
#include "test_utils.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if defined(SINTRA_TEST_DISABLE_SOURCE_LOCATION) && defined(__cpp_lib_source_location)
#error "source_location feature detection was unexpectedly restored"
#endif

namespace {

#if defined(__cpp_lib_source_location) && __cpp_lib_source_location >= 201907L
constexpr bool k_expect_site = true;
#else
constexpr bool k_expect_site = false;
#endif
#ifdef SINTRA_TEST_DISABLE_SOURCE_LOCATION
static_assert(!k_expect_site, "fallback target must exercise missing source_location");
#endif

template <typename Container>
struct Observed_container : Container
{
    auto& contents() { return this->m_c; }
    auto& lock() { return this->m_sl; }

    void install_exited_owner()
    {
        // Another token for our own PID is deterministically an earlier instance.
        // Recovery emits the real diagnostic without an abort or timed contention.
        this->m_sl.test_install_owner(
            sintra::detail::current_process_instance() ^ uint64_t(1), 1);
    }
};

struct Expected_call
{
    unsigned line;
    bool operation_ok;
};

template <typename Container, typename Operation>
bool verify_call(const std::filesystem::path& path, const char* name,
    Container& container, Operation&& operation)
{
    container.install_exited_owner();
    Expected_call expected{};
    {
        sintra::test::Native_stderr_capture capture(path);
        if (!capture.active()) {
            std::fprintf(stderr, "spinlocked_caller_site_test: cannot capture stderr\n");
            return false;
        }
        expected = operation();
    }
    std::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const std::string site = " Waiter at spinlocked_caller_site_test.cpp:" +
        std::to_string(expected.line) + ".";
    const bool location_ok = k_expect_site
        ? text.find(site) != std::string::npos
        : text.find(" Waiter at ") == std::string::npos;
    const bool recovery_ok = text.find("Exited owner PID ") != std::string::npos;
    if (!location_ok || !recovery_ok || !expected.operation_ok) {
        std::fprintf(stderr,
            "spinlocked_caller_site_test: %s: operation=%d recovery=%d site=%d "
            "expected_line=%u\n%s", name, expected.operation_ok, recovery_ok,
            location_ok, expected.line, text.c_str());
        return false;
    }
    return true;
}

template <typename Container>
auto forwarded_size(const Container& container,
    const sintra::detail::spinlock_source_location& where =
        sintra::detail::spinlock_source_location::current())
{
    return container.size(where);
}

bool run_cases(const std::filesystem::path& path)
{
    bool passed = true;
    Observed_container<sintra::spinlocked_deque<int>> sequence;
    sequence.contents() = {1, 2, 3};
    passed &= verify_call(path, "scoped", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        auto access = sequence.scoped();
        access.get().push_back(4);
        return Expected_call{line, access.get().back() == 4};
    });
    passed &= verify_call(path, "const scoped", sequence, [&] {
        const auto& const_sequence = sequence;
        const unsigned line = __LINE__ + 1;
        auto access = const_sequence.scoped();
        return Expected_call{line, access.get().size() == 4};
    });
    passed &= verify_call(path, "with_lock", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        const int result = sequence.with_lock([](auto& values) { values.push_back(5); return values.back(); });
        return Expected_call{line, result == 5};
    });
    passed &= verify_call(path, "size", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        const auto count = sequence.size();
        return Expected_call{line, count == 5};
    });
    passed &= verify_call(path, "explicitly forwarded size", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        const auto count = forwarded_size(sequence);
        return Expected_call{line, count == 5};
    });
    passed &= verify_call(path, "empty", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        const bool empty = sequence.empty();
        return Expected_call{line, !empty};
    });
    passed &= verify_call(path, "pop_front", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        sequence.pop_front();
        return Expected_call{line, sequence.contents().front() == 2};
    });
    passed &= verify_call(path, "clear", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        sequence.clear();
        return Expected_call{line, sequence.contents().empty()};
    });
    passed &= verify_call(path, "scoped_access constructor", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        decltype(sequence)::scoped_access access(sequence.lock(), sequence.contents());
        access.get().push_back(8);
        return Expected_call{line, access.get().front() == 8};
    });
    passed &= verify_call(path, "const_scoped_access constructor", sequence, [&] {
        const unsigned line = __LINE__ + 1;
        decltype(sequence)::const_scoped_access access(sequence.lock(), sequence.contents());
        return Expected_call{line, access.get().front() == 8};
    });

    Observed_container<sintra::spinlocked_umap<int, std::vector<int>>> map;
    passed &= verify_call(path, "set_value", map, [&] {
        const unsigned line = __LINE__ + 1;
        map.set_value(7, std::vector<int>{4, 5});
        return Expected_call{line, map.contents().at(7) == std::vector<int>({4, 5})};
    });
    passed &= verify_call(path, "contains_key", map, [&] {
        const unsigned line = __LINE__ + 1;
        const bool found = map.contains_key(7);
        return Expected_call{line, found};
    });
    passed &= verify_call(path, "copy_value", map, [&] {
        std::vector<int> values;
        const unsigned line = __LINE__ + 1;
        const bool copied = map.copy_value(7, values);
        return Expected_call{line, copied && values == std::vector<int>({4, 5})};
    });
    return passed;
}

} // namespace

int main()
{
    const auto scratch = sintra::test::unique_scratch_directory("spinlocked_caller_site");
    const bool passed = run_cases(scratch / "stderr.txt");
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    if (passed) {
        std::fprintf(stderr, "spinlocked_caller_site_test: named callers passed (%s source_location)\n",
            k_expect_site ? "with" : "without");
    }
    return passed ? 0 : 1;
}
