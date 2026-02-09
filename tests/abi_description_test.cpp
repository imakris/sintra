#include <sintra/detail/type_utils.h>

#include "test_utils.h"

#include <iostream>
#include <string>

namespace {

constexpr std::string_view k_failure_prefix = "abi_description_test: ";

void test_abi_description()
{
    auto desc = sintra::detail::abi_description();
    sintra::test::require_true(!desc.empty(), k_failure_prefix, "abi_description should not be empty");
    sintra::test::require_true(desc.find("compiler") != std::string::npos,
        k_failure_prefix,
        "abi_description should contain 'compiler'");
    sintra::test::require_true(desc.find("standard library") != std::string::npos,
        k_failure_prefix,
        "abi_description should contain 'standard library'");
    sintra::test::require_true(desc.find("platform") != std::string::npos,
        k_failure_prefix,
        "abi_description should contain 'platform'");
    sintra::test::require_true(desc.find("architecture") != std::string::npos,
        k_failure_prefix,
        "abi_description should contain 'architecture'");
}

void test_describe_token_component()
{
    using sintra::detail::detail_type_utils::describe_token_component;

    sintra::test::require_true(describe_token_component("compiler=msvc-1939") == "compiler msvc-1939",
        k_failure_prefix,
        "describe_token_component should format compiler");
    sintra::test::require_true(describe_token_component("stdlib=libstdc++") == "standard library libstdc++",
        k_failure_prefix,
        "describe_token_component should format stdlib");
    sintra::test::require_true(describe_token_component("platform=linux") == "platform linux",
        k_failure_prefix,
        "describe_token_component should format platform");
    sintra::test::require_true(describe_token_component("arch=x64") == "architecture x64",
        k_failure_prefix,
        "describe_token_component should format arch");
    sintra::test::require_true(describe_token_component("unknown=value") == "unknown value",
        k_failure_prefix,
        "describe_token_component should handle unknown keys");
    sintra::test::require_true(describe_token_component("noequals") == "noequals",
        k_failure_prefix,
        "describe_token_component should handle missing equals");
}

void test_describe_abi_token()
{
    using sintra::detail::describe_abi_token;

    sintra::test::require_true(describe_abi_token("") == "(unspecified ABI)",
        k_failure_prefix,
        "describe_abi_token should handle empty token");

    auto result = describe_abi_token("compiler=test-1;stdlib=test-2");
    sintra::test::require_true(result.find("compiler test-1") != std::string::npos,
        k_failure_prefix,
        "describe_abi_token should include compiler");
    sintra::test::require_true(result.find("standard library test-2") != std::string::npos,
        k_failure_prefix,
        "describe_abi_token should include stdlib");
}

void test_abi_token()
{
    auto token = sintra::detail::abi_token();
    sintra::test::require_true(!token.empty(), k_failure_prefix, "abi_token should not be empty");
    sintra::test::require_true(token.find("compiler=") != std::string::npos,
        k_failure_prefix,
        "abi_token should contain 'compiler='");
    sintra::test::require_true(token.find("stdlib=") != std::string::npos,
        k_failure_prefix,
        "abi_token should contain 'stdlib='");
    sintra::test::require_true(token.find("platform=") != std::string::npos,
        k_failure_prefix,
        "abi_token should contain 'platform='");
    sintra::test::require_true(token.find("arch=") != std::string::npos,
        k_failure_prefix,
        "abi_token should contain 'arch='");
}

} // namespace

int main()
{
    try {
        test_abi_description();
        test_describe_token_component();
        test_describe_abi_token();
        test_abi_token();
    }
    catch (const std::exception& ex) {
        std::cerr << "abi_description_test failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
