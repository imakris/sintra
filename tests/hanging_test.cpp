#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool hanging_mode_enabled()
{
    const char* raw = std::getenv("SINTRA_ENABLE_HANGING_TEST");
    if (!raw) {
        return false;
    }

    std::string value(raw);
    if (value.empty()) {
        return false;
    }

    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        return false;
    }

    return true;
}

}

int main()
{
    if (!hanging_mode_enabled()) {
        std::cout << "SINTRA_ENABLE_HANGING_TEST unset - exiting immediately." << std::endl;
        return 0;
    }

    std::cout << "Hanging test started. This test will run indefinitely." << std::endl;
    while (true) {
        // Sleep for a second to prevent this process from consuming 100% CPU.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
