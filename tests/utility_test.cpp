#include <sintra/detail/utility.h>
#include <sintra/detail/ipc/spinlocked_containers.h>

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require_true(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_adaptive_function_basic()
{
    int call_count = 0;
    sintra::Adaptive_function af([&call_count]() {
        ++call_count;
    });

    af();
    require_true(call_count == 1, "Adaptive_function should call the function");

    af();
    require_true(call_count == 2, "Adaptive_function should be callable multiple times");
}

void test_adaptive_function_copy_constructor()
{
    int call_count = 0;
    sintra::Adaptive_function af1([&call_count]() {
        ++call_count;
    });

    sintra::Adaptive_function af2(af1);

    af1();
    require_true(call_count == 1, "Original should work after copy");

    af2();
    require_true(call_count == 2, "Copy should work");
}

void test_adaptive_function_copy_assignment()
{
    int count1 = 0;
    int count2 = 0;

    sintra::Adaptive_function af1([&count1]() { ++count1; });
    sintra::Adaptive_function af2([&count2]() { ++count2; });

    af2 = af1;

    af1();
    require_true(count1 == 1, "Original should work after assignment");

    af2();
    require_true(count1 == 2, "Assigned function should call original's function");
    require_true(count2 == 0, "Original function of af2 should not be called");
}

void test_cstring_vector_from_lvalue()
{
    std::vector<std::string> strings = {"hello", "world", "test"};
    sintra::cstring_vector csv(strings);

    require_true(csv.size() == 3, "cstring_vector size should match input");

    const char* const* data = csv.v();
    require_true(data != nullptr, "cstring_vector data should not be null");
    require_true(std::string(data[0]) == "hello", "First element should match");
    require_true(std::string(data[1]) == "world", "Second element should match");
    require_true(std::string(data[2]) == "test", "Third element should match");
}

void test_cstring_vector_from_rvalue()
{
    std::vector<std::string> strings = {"foo", "bar"};
    sintra::cstring_vector csv(std::move(strings));

    require_true(csv.size() == 2, "cstring_vector size should match input");

    const char* const* data = csv.v();
    require_true(data != nullptr, "cstring_vector data should not be null");
    require_true(std::string(data[0]) == "foo", "First element should match");
    require_true(std::string(data[1]) == "bar", "Second element should match");
}

void test_cstring_vector_empty()
{
    std::vector<std::string> empty;
    sintra::cstring_vector csv(empty);

    require_true(csv.size() == 0, "Empty cstring_vector should have size 0");
}

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
    require_true(scoped.get().size() == 2, "Map should have 2 entries after erase");
    require_true(scoped.get().find("two") == scoped.get().end(), "'two' should be erased");
    require_true(scoped.get().find("one") != scoped.get().end(), "'one' should remain");
    require_true(scoped.get().find("three") != scoped.get().end(), "'three' should remain");
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
        test_spinlocked_umap_scoped_erase();
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "utility_test failed: %s\n", ex.what());
        return 1;
    }

    std::fprintf(stderr, "utility_test passed\n");
    return 0;
}
