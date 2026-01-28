#include <sintra/detail/utility.h>

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
    sintra::detail::Adaptive_function af([&call_count]() {
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
    sintra::detail::Adaptive_function af1([&call_count]() {
        ++call_count;
    });

    sintra::detail::Adaptive_function af2(af1);

    af1();
    require_true(call_count == 1, "Original should work after copy");

    af2();
    require_true(call_count == 2, "Copy should work");
}

void test_adaptive_function_copy_assignment()
{
    int count1 = 0;
    int count2 = 0;

    sintra::detail::Adaptive_function af1([&count1]() { ++count1; });
    sintra::detail::Adaptive_function af2([&count2]() { ++count2; });

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
    sintra::detail::cstring_vector csv(strings);

    require_true(csv.size() == 3, "cstring_vector size should match input");

    const char* const* data = csv.data();
    require_true(data != nullptr, "cstring_vector data should not be null");
    require_true(std::string(data[0]) == "hello", "First element should match");
    require_true(std::string(data[1]) == "world", "Second element should match");
    require_true(std::string(data[2]) == "test", "Third element should match");
}

void test_cstring_vector_from_rvalue()
{
    std::vector<std::string> strings = {"foo", "bar"};
    sintra::detail::cstring_vector csv(std::move(strings));

    require_true(csv.size() == 2, "cstring_vector size should match input");

    const char* const* data = csv.data();
    require_true(data != nullptr, "cstring_vector data should not be null");
    require_true(std::string(data[0]) == "foo", "First element should match");
    require_true(std::string(data[1]) == "bar", "Second element should match");
}

void test_cstring_vector_empty()
{
    std::vector<std::string> empty;
    sintra::detail::cstring_vector csv(empty);

    require_true(csv.size() == 0, "Empty cstring_vector should have size 0");
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
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "utility_test failed: %s\n", ex.what());
        return 1;
    }

    std::fprintf(stderr, "utility_test passed\n");
    return 0;
}
