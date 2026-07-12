#pragma once

#include <sintra/detail/ipc/process_utils.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace sintra::test::managed_child {

inline bool exact_process_is_live(int pid, std::uint64_t start_stamp)
{
    if (pid <= 0 || !sintra::is_process_alive(static_cast<std::uint32_t>(pid))) {
        return false;
    }
    const auto observed =
        sintra::query_process_start_stamp(static_cast<std::uint32_t>(pid));
    return observed && *observed == start_stamp;
}

inline bool write_complete_file(
    const std::filesystem::path& path,
    const std::string&           contents)
{
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << contents;
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace sintra::test::managed_child
