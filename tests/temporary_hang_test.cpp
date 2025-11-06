// TEMPORARY TEST - TO BE REMOVED
// This test intentionally hangs to verify timeout and stack capture works on all platforms
// See: https://github.com/imakris/sintra/issues/XXX

#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Temporary hang test starting..." << std::endl;
    std::cout << "This test will intentionally hang to verify timeout capture." << std::endl;
    std::cout.flush();
    
    // Small delay to let things settle
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    std::cout << "[  FAILED  ] Entering infinite loop..." << std::endl;
    std::cout.flush();
    
    // Infinite loop - will trigger timeout
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}
