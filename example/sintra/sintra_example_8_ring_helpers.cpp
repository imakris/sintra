#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include "sintra/detail/ipc/rings.h"

int main()
{
    try {
        using element_t = std::uint32_t;

        const std::size_t requested = 1000;
        const std::size_t capacity = sintra::aligned_capacity<element_t>(requested);
        if (capacity == 0) {
            std::cerr << "aligned_capacity failed for request " << requested << std::endl;
            return 1;
        }

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        std::ostringstream dir_name;
        dir_name << "sintra_ring_helpers_example_" << ticks;

        const auto directory = std::filesystem::temp_directory_path() / dir_name.str();
        std::filesystem::create_directories(directory);

        sintra::Ring_W<element_t> writer(directory.string(), "ring_helpers_example", capacity);
        sintra::Ring_R<element_t> reader(directory.string(), "ring_helpers_example", capacity, 3);

        const element_t payload[] = {1, 2, 3};
        writer.write_commit(payload, 3);

        auto snapshot = sintra::make_snapshot(reader, 3);
        if (!snapshot) {
            std::cerr << "Unexpected read range size" << std::endl;
            return 1;
        }
        auto range = snapshot.range();
        if (range.end - range.begin != 3) {
            std::cerr << "Unexpected read range size" << std::endl;
            return 1;
        }

        std::cout << "Aligned capacity: " << capacity << std::endl;
        std::cout << "Payload:";
        for (auto it = range.begin; it != range.end; ++it) {
            std::cout << ' ' << *it;
        }
        std::cout << std::endl;

        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Ring helpers example failed: " << ex.what() << std::endl;
        return 1;
    }
}
