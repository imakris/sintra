#include <sintra/detail/utility.h>
#include <sintra/detail/ipc/spinlocked_containers.h>

#include "test_utils.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr std::string_view k_failure_prefix = "utility_test: ";

void test_adaptive_function_basic()
{
    int call_count = 0;
    sintra::Adaptive_function af([&call_count]() {
        ++call_count;
    });

    af();
    sintra::test::require_true(call_count == 1,
                               k_failure_prefix,
                               "Adaptive_function should call the function");

    af();
    sintra::test::require_true(call_count == 2,
                               k_failure_prefix,
                               "Adaptive_function should be callable multiple times");
}

void test_adaptive_function_copy_constructor()
{
    int call_count = 0;
    sintra::Adaptive_function af1([&call_count]() {
        ++call_count;
    });

    sintra::Adaptive_function af2(af1);

    af1();
    sintra::test::require_true(call_count == 1,
                               k_failure_prefix,
                               "Original should work after copy");

    af2();
    sintra::test::require_true(call_count == 2,
                               k_failure_prefix,
                               "Copy should work");
}

void test_adaptive_function_copy_assignment()
{
    int count1 = 0;
    int count2 = 0;

    sintra::Adaptive_function af1([&count1]() { ++count1; });
    sintra::Adaptive_function af2([&count2]() { ++count2; });

    af2 = af1;

    af1();
    sintra::test::require_true(count1 == 1,
                               k_failure_prefix,
                               "Original should work after assignment");

    af2();
    sintra::test::require_true(count1 == 2,
                               k_failure_prefix,
                               "Assigned function should call original's function");
    sintra::test::require_true(count2 == 0,
                               k_failure_prefix,
                               "Original function of af2 should not be called");
}

void test_cstring_vector_from_lvalue()
{
    std::vector<std::string> strings = {"hello", "world", "test"};
    sintra::cstring_vector csv(strings);

    sintra::test::require_true(csv.size() == 3,
                               k_failure_prefix,
                               "cstring_vector size should match input");

    const char* const* data = csv.v();
    sintra::test::require_true(data != nullptr,
                               k_failure_prefix,
                               "cstring_vector data should not be null");
    sintra::test::require_true(std::string(data[0]) == "hello",
                               k_failure_prefix,
                               "First element should match");
    sintra::test::require_true(std::string(data[1]) == "world",
                               k_failure_prefix,
                               "Second element should match");
    sintra::test::require_true(std::string(data[2]) == "test",
                               k_failure_prefix,
                               "Third element should match");
}

void test_cstring_vector_from_rvalue()
{
    std::vector<std::string> strings = {"foo", "bar"};
    sintra::cstring_vector csv(std::move(strings));

    sintra::test::require_true(csv.size() == 2,
                               k_failure_prefix,
                               "cstring_vector size should match input");

    const char* const* data = csv.v();
    sintra::test::require_true(data != nullptr,
                               k_failure_prefix,
                               "cstring_vector data should not be null");
    sintra::test::require_true(std::string(data[0]) == "foo",
                               k_failure_prefix,
                               "First element should match");
    sintra::test::require_true(std::string(data[1]) == "bar",
                               k_failure_prefix,
                               "Second element should match");
}

void test_cstring_vector_empty()
{
    std::vector<std::string> empty;
    sintra::cstring_vector csv(empty);

    sintra::test::require_true(csv.size() == 0,
                               k_failure_prefix,
                               "Empty cstring_vector should have size 0");
}

#ifndef _WIN32
void test_env_key_of()
{
    using sintra::detail::env_key_of;

    sintra::test::require_true(env_key_of("FOO=bar") == "FOO",
                               k_failure_prefix,
                               "env_key_of should split at '='");
    sintra::test::require_true(env_key_of("NOEQ") == "NOEQ",
                               k_failure_prefix,
                               "env_key_of should return whole string when no '='");
    sintra::test::require_true(env_key_of("A=B=C") == "A",
                               k_failure_prefix,
                               "env_key_of should split at first '='");
}

void test_build_environment_entries()
{
    using sintra::detail::build_environment_entries;
    using sintra::detail::env_key_of;

    const std::string key = "SINTRA_TEST_ENV_KEY";
    const std::string override_entry = key + "=OVERRIDE";

    setenv(key.c_str(), "ORIGINAL", 1);

    auto env_before = build_environment_entries({});
    sintra::test::require_true(!env_before.empty(),
                               k_failure_prefix,
                               "build_environment_entries should return environment entries");

    auto env_after = build_environment_entries({override_entry});

    int match_count = 0;
    bool found_override = false;
    for (const auto& entry : env_after) {
        if (env_key_of(entry) == key) {
            ++match_count;
            if (entry == override_entry) {
                found_override = true;
            }
        }
    }

    sintra::test::require_true(match_count == 1,
                               k_failure_prefix,
                               "override should replace existing entry exactly once");
    sintra::test::require_true(found_override,
                               k_failure_prefix,
                               "override entry should be present");
}
#endif

void test_spinlocked_umap_scoped_erase()
{
    // Test scoped_access::erase(iterator) - exercises the uncovered code path
    sintra::detail::spinlocked<std::unordered_map, std::string, int> map;

    // Insert some entries
    map.emplace("one", 1);
    map.emplace("two", 2);
    map.emplace("three", 3);

    // Use scoped access to iterate and erase
    {
        auto scoped = map.scoped();
        for (auto it = scoped.begin(); it != scoped.end(); ) {
            if (it->second == 2) {
                it = scoped.erase(it);  // This is the uncovered line!
            } else {
                ++it;
            }
        }
    }

    // Verify the entry was erased
    auto scoped = map.scoped();
    sintra::test::require_true(scoped.get().size() == 2,
                               k_failure_prefix,
                               "Map should have 2 entries after erase");
    sintra::test::require_true(scoped.get().find("two") == scoped.get().end(),
                               k_failure_prefix,
                               "'two' should be erased");
    sintra::test::require_true(scoped.get().find("one") != scoped.get().end(),
                               k_failure_prefix,
                               "'one' should remain");
    sintra::test::require_true(scoped.get().find("three") != scoped.get().end(),
                               k_failure_prefix,
                               "'three' should remain");
}

} // namespace

int main()
{
    try {
        test_adaptive_function_basic();
        test_adaptive_function_copy_constructor();
        test_adaptive_function_copy_assignment();
        test_cstring_vector_from_lvalue();
        test_cstring_vector_from_rvalue();
        test_cstring_vector_empty();
#ifndef _WIN32
        test_env_key_of();
        test_build_environment_entries();
#endif
        test_spinlocked_umap_scoped_erase();
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "utility_test failed: %s\n", ex.what());
        return 1;
    }

    std::fprintf(stderr, "utility_test passed\n");
    return 0;
}
