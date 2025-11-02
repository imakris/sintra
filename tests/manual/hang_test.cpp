// hang_test.cpp
// A test that intentionally hangs indefinitely.
// Useful for testing timeout mechanisms and debugging hang scenarios.

#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "hang_test: Starting intentional hang...\n" << std::flush;

    // Infinite loop - test will hang here
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
