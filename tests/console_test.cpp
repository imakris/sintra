#include <sintra/sintra.h>

#include <cstdio>
#include <stdexcept>

namespace {

void test_console_basic()
{
    // Test basic console output - destructor calls Coordinator::rpc_print
    sintra::console() << "Console test message";
}

void test_console_multiple_insertions()
{
    // Test multiple insertions into single console instance
    sintra::console() << "Value: " << 42 << ", String: " << "test";
}

void test_console_separate_instances()
{
    // Test multiple separate console instances
    sintra::console() << "First message";
    sintra::console() << "Second message";
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

    test_console_basic();
    test_console_multiple_insertions();
    test_console_separate_instances();

    sintra::finalize();
    std::fprintf(stderr, "Console test passed\n");
    return 0;
}
