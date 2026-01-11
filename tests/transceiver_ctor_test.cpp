//
// Sintra Transceiver Constructor Test
//
// This test validates the transceiver constructors introduced in recent changes:
// - const char* constructor (2a18bee)
// - nullptr handling for const char* constructor
// - string constructor (existing)
//
// The test verifies:
// - Construction with string name works
// - Construction with const char* name works
// - Construction with nullptr name works (treated as empty string)
// - Construction with empty string works
// - Named transceivers can be published and resolved via Coordinator RPC
//

#include <sintra/sintra.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

struct Test_bus : sintra::Derived_transceiver<Test_bus>
{
    using Derived_transceiver::Derived_transceiver;

    int get_value() const { return 42; }
    SINTRA_RPC(get_value)
};

bool verify_resolve(const char* name,
                    sintra::instance_id_type expected,
                    const char* context)
{
    const auto resolved = sintra::Coordinator::rpc_resolve_instance(
        s_coord_id,
        name);

    if (resolved == sintra::invalid_instance_id) {
        std::fprintf(stderr, "%s: failed to resolve instance by name '%s'\n",
                     context, name);
        return false;
    }

    if (resolved != expected) {
        std::fprintf(stderr, "%s: resolved ID %llu does not match expected %llu\n",
                     context,
                     (unsigned long long)resolved,
                     (unsigned long long)expected);
        return false;
    }

    return true;
}

int test_string_constructor()
{
    // Test construction with std::string
    std::string name = "string_named_bus";
    Test_bus bus(name);

    if (bus.instance_id() == sintra::invalid_instance_id) {
        std::fprintf(stderr, "String constructor: invalid instance id\n");
        return 1;
    }

    if (!verify_resolve(name.c_str(), bus.instance_id(), "String constructor")) {
        return 1;
    }

    return 0;
}

int test_const_char_constructor()
{
    // Test construction with const char*
    const char* name = "const_char_named_bus";
    Test_bus bus(name);

    if (bus.instance_id() == sintra::invalid_instance_id) {
        std::fprintf(stderr, "const char* constructor: invalid instance id\n");
        return 1;
    }

    if (!verify_resolve(name, bus.instance_id(), "const char* constructor")) {
        return 1;
    }

    return 0;
}

int test_nullptr_constructor()
{
    // Test construction with nullptr (should be treated as empty string)
    const char* name = nullptr;
    Test_bus bus(name);

    if (bus.instance_id() == sintra::invalid_instance_id) {
        std::fprintf(stderr, "nullptr constructor: invalid instance id\n");
        return 1;
    }

    if (!verify_resolve("literal_named_bus",
                        bus.instance_id(),
                        "Literal constructor")) {
        return 1;
    }

    return 0;
}

int test_empty_string_constructor()
{
    // Test construction with empty string
    Test_bus bus("");

    if (bus.instance_id() == sintra::invalid_instance_id) {
        std::fprintf(stderr, "Empty string constructor: invalid instance id\n");
        return 1;
    }

    return 0;
}

int test_literal_constructor()
{
    // Test construction with string literal directly
    Test_bus bus("literal_named_bus");

    if (bus.instance_id() == sintra::invalid_instance_id) {
        std::fprintf(stderr, "Literal constructor: invalid instance id\n");
        return 1;
    }

    return 0;
}

int test_named_transceiver_publish_and_resolve()
{
    // Test that a named transceiver can be published and resolved
    const char* bus_name = "publishable_resolvable_bus";
    Test_bus bus;

    if (!bus.assign_name(bus_name)) {
        std::fprintf(stderr, "Failed to assign name to bus\n");
        return 1;
    }

    // Verify the instance can be resolved via Coordinator RPC
    const auto resolved = sintra::Coordinator::rpc_resolve_instance(
        s_coord_id,
        bus_name);

    if (resolved == sintra::invalid_instance_id) {
        std::fprintf(stderr, "Failed to resolve instance by name '%s'\n", bus_name);
        return 1;
    }

    // Verify the resolved ID matches the bus's instance ID
    if (resolved != bus.instance_id()) {
        std::fprintf(stderr, "Resolved ID %llu does not match bus instance ID %llu\n",
                     (unsigned long long)resolved,
                     (unsigned long long)bus.instance_id());
        return 1;
    }

    std::fprintf(stderr, "Successfully resolved '%s' to instance ID %llu\n",
                 bus_name, (unsigned long long)resolved);

    return 0;
}

int test_const_char_with_name_and_resolve()
{
    // Test that const char* constructor creates a transceiver that can be named and resolved
    const char* initial_name = nullptr;  // Empty name at construction
    Test_bus bus(initial_name);

    if (bus.instance_id() == sintra::invalid_instance_id) {
        std::fprintf(stderr, "const char* nullptr constructor: invalid instance id\n");
        return 1;
    }

    const char* assigned_name = "const_char_resolvable_bus";
    if (!bus.assign_name(assigned_name)) {
        std::fprintf(stderr, "Failed to assign name after nullptr construction\n");
        return 1;
    }

    // Verify resolution
    const auto resolved = sintra::Coordinator::rpc_resolve_instance(
        s_coord_id,
        assigned_name);

    if (resolved != bus.instance_id()) {
        std::fprintf(stderr, "Resolution failed for const char* constructed transceiver\n");
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, const_cast<const char* const*>(argv));
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to initialize sintra: %s\n", e.what());
        return 1;
    }

    int result = 0;

    result = test_string_constructor();
    if (result != 0) {
        std::fprintf(stderr, "test_string_constructor failed\n");
        sintra::finalize();
        return result;
    }
    std::fprintf(stderr, "test_string_constructor passed\n");

    result = test_const_char_constructor();
    if (result != 0) {
        std::fprintf(stderr, "test_const_char_constructor failed\n");
        sintra::finalize();
        return result;
    }
    std::fprintf(stderr, "test_const_char_constructor passed\n");

    result = test_nullptr_constructor();
    if (result != 0) {
        std::fprintf(stderr, "test_nullptr_constructor failed\n");
        sintra::finalize();
        return result;
    }
    std::fprintf(stderr, "test_nullptr_constructor passed\n");

    result = test_empty_string_constructor();
    if (result != 0) {
        std::fprintf(stderr, "test_empty_string_constructor failed\n");
        sintra::finalize();
        return result;
    }
    std::fprintf(stderr, "test_empty_string_constructor passed\n");

    result = test_literal_constructor();
    if (result != 0) {
        std::fprintf(stderr, "test_literal_constructor failed\n");
        sintra::finalize();
        return result;
    }
    std::fprintf(stderr, "test_literal_constructor passed\n");

    result = test_named_transceiver_publish_and_resolve();
    if (result != 0) {
        std::fprintf(stderr, "test_named_transceiver_publish_and_resolve failed\n");
        sintra::finalize();
        return result;
    }
    std::fprintf(stderr, "test_named_transceiver_publish_and_resolve passed\n");

    result = test_const_char_with_name_and_resolve();
    if (result != 0) {
        std::fprintf(stderr, "test_const_char_with_name_and_resolve failed\n");
        sintra::finalize();
        return result;
    }
    std::fprintf(stderr, "test_const_char_with_name_and_resolve passed\n");

    sintra::finalize();
    std::fprintf(stderr, "All transceiver constructor tests passed\n");
    return 0;
}
