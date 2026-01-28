// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>
#include <sintra/detail/exception_conversions_impl.h>

#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <typeinfo>
#include <variant>

namespace {

// Helper macro to test that string_to_exception throws the expected exception type
#define TEST_THROWS(exception_type_id, expected_exception_type, test_message)        \
    do {                                                                              \
        bool caught_expected = false;                                                 \
        try {                                                                         \
            sintra::string_to_exception(exception_type_id, test_message);             \
        }                                                                             \
        catch (const expected_exception_type& e) {                                    \
            caught_expected = true;                                                   \
            if (std::string(e.what()) != test_message) {                              \
                std::cerr << "Exception message mismatch for " #expected_exception_type \
                          << ": expected '" << test_message << "', got '" << e.what() \
                          << "'" << std::endl;                                        \
                return 1;                                                             \
            }                                                                         \
        }                                                                             \
        catch (...) {                                                                 \
            std::cerr << "Unexpected exception type for " #expected_exception_type    \
                      << std::endl;                                                   \
            return 1;                                                                 \
        }                                                                             \
        if (!caught_expected) {                                                       \
            std::cerr << "No exception thrown for " #expected_exception_type          \
                      << std::endl;                                                   \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

// Helper macro to test that string_to_exception throws expected type and message contains test_message
#define TEST_THROWS_CONTAINS(exception_type_id, expected_exception_type, test_message) \
    do {                                                                               \
        bool caught_expected = false;                                                  \
        try {                                                                          \
            sintra::string_to_exception(exception_type_id, test_message);              \
        }                                                                              \
        catch (const expected_exception_type& e) {                                     \
            caught_expected = true;                                                    \
            if (std::string(e.what()).find(test_message) == std::string::npos) {       \
                std::cerr << "Exception message mismatch for " #expected_exception_type \
                          << ": expected message to contain '" << test_message         \
                          << "', got '" << e.what() << "'" << std::endl;               \
                return 1;                                                              \
            }                                                                          \
        }                                                                              \
        catch (...) {                                                                  \
            std::cerr << "Unexpected exception type for " #expected_exception_type     \
                      << std::endl;                                                    \
            return 1;                                                                  \
        }                                                                              \
        if (!caught_expected) {                                                        \
            std::cerr << "No exception thrown for " #expected_exception_type           \
                      << std::endl;                                                    \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

// Test exceptions via the reserved_id switch statement path
int test_reserved_id_exceptions()
{
    using sintra::detail::reserved_id;

    // Test std::invalid_argument via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_invalid_argument),
        std::invalid_argument,
        "test invalid_argument via reserved_id"
    );

    // Test std::domain_error via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_domain_error),
        std::domain_error,
        "test domain_error via reserved_id"
    );

    // Test std::length_error via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_length_error),
        std::length_error,
        "test length_error via reserved_id"
    );

    // Test std::out_of_range via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_out_of_range),
        std::out_of_range,
        "test out_of_range via reserved_id"
    );

    // Test std::range_error via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_range_error),
        std::range_error,
        "test range_error via reserved_id"
    );

    // Test std::overflow_error via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_overflow_error),
        std::overflow_error,
        "test overflow_error via reserved_id"
    );

    // Test std::underflow_error via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_underflow_error),
        std::underflow_error,
        "test underflow_error via reserved_id"
    );

    // Test std::ios_base::failure via reserved_id
    TEST_THROWS_CONTAINS(
        static_cast<sintra::type_id_type>(reserved_id::std_ios_base_failure),
        std::ios_base::failure,
        "test ios_base::failure via reserved_id"
    );

    // Test std::logic_error via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_logic_error),
        std::logic_error,
        "test logic_error via reserved_id"
    );

    // Test std::runtime_error via reserved_id
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_runtime_error),
        std::runtime_error,
        "test runtime_error via reserved_id"
    );

    // Test std::exception via reserved_id (maps to throw_generic_exception -> std::runtime_error)
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::std_exception),
        std::runtime_error,
        "test std_exception via reserved_id"
    );

    // Test unknown_exception via reserved_id (maps to throw_generic_exception -> std::runtime_error)
    TEST_THROWS(
        static_cast<sintra::type_id_type>(reserved_id::unknown_exception),
        std::runtime_error,
        "test unknown_exception via reserved_id"
    );

    return 0;
}

// Test exceptions via the ex_map lookup path (using get_type_id)
int test_type_id_exceptions()
{
    // Test std::logic_error via type_id
    TEST_THROWS(
        sintra::get_type_id<std::logic_error>(),
        std::logic_error,
        "test logic_error via type_id"
    );

    // Test std::invalid_argument via type_id
    TEST_THROWS(
        sintra::get_type_id<std::invalid_argument>(),
        std::invalid_argument,
        "test invalid_argument via type_id"
    );

    // Test std::domain_error via type_id
    TEST_THROWS(
        sintra::get_type_id<std::domain_error>(),
        std::domain_error,
        "test domain_error via type_id"
    );

    // Test std::length_error via type_id
    TEST_THROWS(
        sintra::get_type_id<std::length_error>(),
        std::length_error,
        "test length_error via type_id"
    );

    // Test std::out_of_range via type_id
    TEST_THROWS(
        sintra::get_type_id<std::out_of_range>(),
        std::out_of_range,
        "test out_of_range via type_id"
    );

    // Test std::runtime_error via type_id
    TEST_THROWS(
        sintra::get_type_id<std::runtime_error>(),
        std::runtime_error,
        "test runtime_error via type_id"
    );

    // Test std::range_error via type_id
    TEST_THROWS(
        sintra::get_type_id<std::range_error>(),
        std::range_error,
        "test range_error via type_id"
    );

    // Test std::overflow_error via type_id
    TEST_THROWS(
        sintra::get_type_id<std::overflow_error>(),
        std::overflow_error,
        "test overflow_error via type_id"
    );

    // Test std::underflow_error via type_id
    TEST_THROWS(
        sintra::get_type_id<std::underflow_error>(),
        std::underflow_error,
        "test underflow_error via type_id"
    );

    // Test std::system_error -> throws std::runtime_error
    TEST_THROWS(
        sintra::get_type_id<std::system_error>(),
        std::runtime_error,
        "test system_error via type_id"
    );

    // Test std::ios_base::failure via type_id
    TEST_THROWS_CONTAINS(
        sintra::get_type_id<std::ios_base::failure>(),
        std::ios_base::failure,
        "test ios_base::failure via type_id"
    );

    // Test exceptions that map to their parent type due to non-trivial constructors
    // std::future_error -> std::logic_error
    TEST_THROWS(
        sintra::get_type_id<std::future_error>(),
        std::logic_error,
        "test future_error via type_id"
    );

    // std::regex_error -> std::runtime_error
    TEST_THROWS(
        sintra::get_type_id<std::regex_error>(),
        std::runtime_error,
        "test regex_error via type_id"
    );

    // std::filesystem::filesystem_error -> std::runtime_error
    TEST_THROWS(
        sintra::get_type_id<std::filesystem::filesystem_error>(),
        std::runtime_error,
        "test filesystem_error via type_id"
    );

    return 0;
}

// Test exceptions that use throw_generic_exception (bad_* types)
int test_generic_exceptions()
{
    // All these bad_* exception types map to throw_generic_exception,
    // which throws std::runtime_error

    // Test std::bad_optional_access
    TEST_THROWS(
        sintra::get_type_id<std::bad_optional_access>(),
        std::runtime_error,
        "test bad_optional_access via type_id"
    );

    // Test std::bad_typeid
    TEST_THROWS(
        sintra::get_type_id<std::bad_typeid>(),
        std::runtime_error,
        "test bad_typeid via type_id"
    );

    // Test std::bad_cast
    TEST_THROWS(
        sintra::get_type_id<std::bad_cast>(),
        std::runtime_error,
        "test bad_cast via type_id"
    );

    // Test std::bad_weak_ptr
    TEST_THROWS(
        sintra::get_type_id<std::bad_weak_ptr>(),
        std::runtime_error,
        "test bad_weak_ptr via type_id"
    );

    // Test std::bad_function_call
    TEST_THROWS(
        sintra::get_type_id<std::bad_function_call>(),
        std::runtime_error,
        "test bad_function_call via type_id"
    );

    // Test std::bad_alloc
    TEST_THROWS(
        sintra::get_type_id<std::bad_alloc>(),
        std::runtime_error,
        "test bad_alloc via type_id"
    );

    // Test std::bad_array_new_length
    TEST_THROWS(
        sintra::get_type_id<std::bad_array_new_length>(),
        std::runtime_error,
        "test bad_array_new_length via type_id"
    );

    // Test std::bad_exception
    TEST_THROWS(
        sintra::get_type_id<std::bad_exception>(),
        std::runtime_error,
        "test bad_exception via type_id"
    );

    // Test std::bad_variant_access
    TEST_THROWS(
        sintra::get_type_id<std::bad_variant_access>(),
        std::runtime_error,
        "test bad_variant_access via type_id"
    );

    return 0;
}

// Test the else branch (unknown type_id)
int test_unknown_type_exception()
{
    // Use a type_id that is not in the reserved_id range and not in the ex_map
    // This should trigger the else branch that calls throw_generic_exception
    struct UnknownType {};

    TEST_THROWS(
        sintra::get_type_id<UnknownType>(),
        std::runtime_error,
        "test unknown type via type_id"
    );

    return 0;
}

// Helper macro to test exception_to_string
#define TEST_EXCEPTION_TO_STRING(exception_type, test_message)                    \
    do {                                                                          \
        exception_type ex(test_message);                                          \
        auto [type_id, msg] = sintra::exception_to_string(ex);                    \
        if (type_id != sintra::get_type_id<exception_type>()) {                   \
            std::cerr << "Type ID mismatch for " #exception_type                  \
                      << ": expected " << sintra::get_type_id<exception_type>()   \
                      << ", got " << type_id << std::endl;                        \
            return 1;                                                             \
        }                                                                         \
        if (msg != test_message) {                                                \
            std::cerr << "Message mismatch for " #exception_type                  \
                      << ": expected '" << test_message << "', got '"             \
                      << msg << "'" << std::endl;                                 \
            return 1;                                                             \
        }                                                                         \
    } while (0)

// Test exception_to_string for std::exception-derived types
int test_exception_to_string()
{
    TEST_EXCEPTION_TO_STRING(std::logic_error, "test logic_error to string");
    TEST_EXCEPTION_TO_STRING(std::invalid_argument, "test invalid_argument to string");
    TEST_EXCEPTION_TO_STRING(std::domain_error, "test domain_error to string");
    TEST_EXCEPTION_TO_STRING(std::length_error, "test length_error to string");
    TEST_EXCEPTION_TO_STRING(std::out_of_range, "test out_of_range to string");
    TEST_EXCEPTION_TO_STRING(std::runtime_error, "test runtime_error to string");
    TEST_EXCEPTION_TO_STRING(std::range_error, "test range_error to string");
    TEST_EXCEPTION_TO_STRING(std::overflow_error, "test overflow_error to string");
    TEST_EXCEPTION_TO_STRING(std::underflow_error, "test underflow_error to string");

    return 0;
}

// Test round-trip: exception_to_string -> string_to_exception
#define TEST_ROUND_TRIP(exception_type, test_message)                             \
    do {                                                                          \
        exception_type original_ex(test_message);                                 \
        auto [type_id, msg] = sintra::exception_to_string(original_ex);           \
        bool caught_expected = false;                                             \
        try {                                                                     \
            sintra::string_to_exception(type_id, msg);                            \
        }                                                                         \
        catch (const exception_type& e) {                                         \
            caught_expected = true;                                               \
            if (std::string(e.what()) != test_message) {                          \
                std::cerr << "Round-trip message mismatch for " #exception_type   \
                          << ": expected '" << test_message << "', got '"         \
                          << e.what() << "'" << std::endl;                        \
                return 1;                                                         \
            }                                                                     \
        }                                                                         \
        catch (...) {                                                             \
            std::cerr << "Round-trip: unexpected exception type for "             \
                      << #exception_type << std::endl;                            \
            return 1;                                                             \
        }                                                                         \
        if (!caught_expected) {                                                   \
            std::cerr << "Round-trip: no exception thrown for " #exception_type   \
                      << std::endl;                                               \
            return 1;                                                             \
        }                                                                         \
    } while (0)

int test_round_trip()
{
    TEST_ROUND_TRIP(std::logic_error, "round-trip logic_error");
    TEST_ROUND_TRIP(std::invalid_argument, "round-trip invalid_argument");
    TEST_ROUND_TRIP(std::domain_error, "round-trip domain_error");
    TEST_ROUND_TRIP(std::length_error, "round-trip length_error");
    TEST_ROUND_TRIP(std::out_of_range, "round-trip out_of_range");
    TEST_ROUND_TRIP(std::runtime_error, "round-trip runtime_error");
    TEST_ROUND_TRIP(std::range_error, "round-trip range_error");
    TEST_ROUND_TRIP(std::overflow_error, "round-trip overflow_error");
    TEST_ROUND_TRIP(std::underflow_error, "round-trip underflow_error");

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    int result = 0;

    result = test_reserved_id_exceptions();
    if (result != 0) {
        std::cerr << "test_reserved_id_exceptions failed" << std::endl;
        sintra::finalize();
        return result;
    }

    result = test_type_id_exceptions();
    if (result != 0) {
        std::cerr << "test_type_id_exceptions failed" << std::endl;
        sintra::finalize();
        return result;
    }

    result = test_generic_exceptions();
    if (result != 0) {
        std::cerr << "test_generic_exceptions failed" << std::endl;
        sintra::finalize();
        return result;
    }

    result = test_unknown_type_exception();
    if (result != 0) {
        std::cerr << "test_unknown_type_exception failed" << std::endl;
        sintra::finalize();
        return result;
    }

    result = test_exception_to_string();
    if (result != 0) {
        std::cerr << "test_exception_to_string failed" << std::endl;
        sintra::finalize();
        return result;
    }

    result = test_round_trip();
    if (result != 0) {
        std::cerr << "test_round_trip failed" << std::endl;
        sintra::finalize();
        return result;
    }

    std::cout << "All exception conversion tests passed" << std::endl;
    sintra::finalize();
    return 0;
}
