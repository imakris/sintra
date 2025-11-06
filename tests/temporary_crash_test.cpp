// TEMPORARY TEST - TO BE REMOVED
// This test intentionally crashes to verify stack capture works on all platforms
// See: https://github.com/imakris/sintra/issues/XXX

#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Temporary crash test starting..." << std::endl;
    std::cout << "This test will intentionally crash to verify stack capture." << std::endl;
    std::cout.flush();
    
    // Give debugger time to prepare
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    std::cout << "[  FAILED  ] Triggering intentional crash..." << std::endl;
    std::cout.flush();
    
    // Small delay before crash
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Intentional segfault
    int* null_ptr = nullptr;
    *null_ptr = 42;  // This should never execute
    
    return 0;
}
