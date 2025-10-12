#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Hanging test started. This test will run indefinitely." << std::endl;
    while (true) {
        // Sleep for a second to prevent this process from consuming 100% CPU.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    // This part of the code is unreachable and is only here to satisfy the compiler.
    return 0;
}