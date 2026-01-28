#include <sintra/detail/type_utils.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require_true(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_abi_description()
{
    auto desc = sintra::detail::abi_description();
    require_true(!desc.empty(), "abi_description should not be empty");
    require_true(desc.find("compiler") != std::string::npos,
        "abi_description should contain 'compiler'");
    require_true(desc.find("standard library") != std::string::npos,
        "abi_description should contain 'standard library'");
    require_true(desc.find("platform") != std::string::npos,
        "abi_description should contain 'platform'");
    require_true(desc.find("architecture") != std::string::npos,
        "abi_description should contain 'architecture'");
}

void test_describe_token_component()
{
    using sintra::detail::detail_type_utils::describe_token_component;

    require_true(describe_token_component("compiler=msvc-1939") == "compiler msvc-1939",
        "describe_token_component should format compiler");
    require_true(describe_token_component("stdlib=libstdc++") == "standard library libstdc++",
        "describe_token_component should format stdlib");
    require_true(describe_token_component("platform=linux") == "platform linux",
        "describe_token_component should format platform");
    require_true(describe_token_component("arch=x64") == "architecture x64",
        "describe_token_component should format arch");
    require_true(describe_token_component("unknown=value") == "unknown value",
        "describe_token_component should handle unknown keys");
    require_true(describe_token_component("noequals") == "noequals",
        "describe_token_component should handle missing equals");
}

void test_describe_abi_token()
{
    using sintra::detail::describe_abi_token;

    require_true(describe_abi_token("") == "(unspecified ABI)",
        "describe_abi_token should handle empty token");

    auto result = describe_abi_token("compiler=test-1;stdlib=test-2");
    require_true(result.find("compiler test-1") != std::string::npos,
        "describe_abi_token should include compiler");
    require_true(result.find("standard library test-2") != std::string::npos,
        "describe_abi_token should include stdlib");
}

void test_abi_token()
{
    auto token = sintra::detail::abi_token();
    require_true(!token.empty(), "abi_token should not be empty");
    require_true(token.find("compiler=") != std::string::npos,
        "abi_token should contain 'compiler='");
    require_true(token.find("stdlib=") != std::string::npos,
        "abi_token should contain 'stdlib='");
    require_true(token.find("platform=") != std::string::npos,
        "abi_token should contain 'platform='");
    require_true(token.find("arch=") != std::string::npos,
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
