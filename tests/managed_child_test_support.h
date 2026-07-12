#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace sintra::test::managed_child {

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
