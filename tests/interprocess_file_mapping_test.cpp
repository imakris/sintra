#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "sintra/detail/ipc/file_mapping.h"
#include "sintra/detail/ipc/platform_utils.h"

#include "test_environment.h"

namespace
{

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "interprocess_file_mapping_test failure: " << message << std::endl;
    std::exit(1);
}

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void expect_error_code(const std::system_error& error,
                       std::errc expected,
                       const std::string& context)
{
    if (error.code() != std::make_error_code(expected)) {
        std::cerr << context << " threw unexpected error: "
                  << error.code().message() << std::endl;
        std::exit(1);
    }
}

std::filesystem::path make_unique_path()
{
    auto dir = sintra::test::scratch_subdirectory("interprocess_file_mapping");
    std::random_device rd;
    for (int attempt = 0; attempt < 128; ++attempt) {
        auto candidate =
            dir / ("sintra_ipc_file_mapping_test_" + std::to_string(rd()) + ".bin");
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    fail("unable to create unique temporary file path");
    return {};
}

struct temporary_file
{
    explicit temporary_file(const std::vector<std::uint8_t>& contents)
        : path(make_unique_path())
    {
        write(contents);
    }

    ~temporary_file()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    void write(const std::vector<std::uint8_t>& contents) const
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            fail("failed to open temporary file for writing");
        }
        file.write(reinterpret_cast<const char*>(contents.data()),
                   static_cast<std::streamsize>(contents.size()));
        if (!file) {
            fail("failed to write temporary file contents");
        }
        file.close();
        if (!file) {
            fail("failed to close temporary file after writing");
        }
    }

    std::vector<std::uint8_t> read() const
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            fail("failed to open temporary file for reading");
        }
        file.seekg(0, std::ios::end);
        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
        if (size > 0) {
            file.read(reinterpret_cast<char*>(buffer.data()), size);
        }
        if (!file) {
            fail("failed to read temporary file contents");
        }
        return buffer;
    }

    std::filesystem::path path;
};

std::vector<std::uint8_t> make_test_data(std::size_t size)
{
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>((i * 37u) & 0xFFu);
    }
    return data;
}

} // namespace

int main()
{
    namespace ipc = sintra::detail::ipc;

    const std::size_t page_size = sintra::system_page_size();
    const std::size_t data_size = page_size * 2 + 128;
    const auto          original_data = make_test_data(data_size);
    temporary_file temp(original_data);

    // Mapping the full file in read-only mode should expose the exact contents.
    {
        ipc::file_mapping mapping(temp.path, ipc::read_only);
        ipc::mapped_region region(mapping, ipc::read_only, 0, 0);
        expect(region.size() == original_data.size(),
               "read-only mapping should cover the full file when size is zero");
        auto* bytes = static_cast<const std::uint8_t*>(region.data());
        expect(bytes != nullptr, "read-only mapping returned null address");
        for (std::size_t i = 0; i < original_data.size(); ++i) {
            if (bytes[i] != original_data[i]) {
                fail("read-only mapping contents mismatch");
            }
        }
    }

    // Partial mapping with an offset should expose the subset of bytes requested.
    {
        const std::size_t offset = page_size;
        const std::size_t length = 128;
        ipc::file_mapping mapping(temp.path, ipc::read_only);
        ipc::mapped_region region(mapping, ipc::read_only, offset, length);
        expect(region.size() == length,
               "partial mapping reported unexpected size");
        auto* bytes = static_cast<const std::uint8_t*>(region.data());
        expect(bytes != nullptr, "partial mapping returned null address");
        for (std::size_t i = 0; i < length; ++i) {
            if (bytes[i] != original_data[offset + i]) {
                fail("partial mapping contents mismatch");
            }
        }
    }

    // Mapping beyond the end of the file should throw invalid_argument.
    {
        ipc::file_mapping mapping(temp.path, ipc::read_only);
        bool threw = false;
        try {
            ipc::mapped_region region(mapping, ipc::read_only,
                                      static_cast<std::uint64_t>(original_data.size()) + 1,
                                      0);
            (void)region;
        } catch (const std::system_error& error) {
            threw = true;
            expect_error_code(error, std::errc::invalid_argument,
                              "offset beyond end of file");
        }
        expect(threw, "mapping beyond end of file should throw");
    }

    // Zero-length mappings are rejected even when the offset equals the file size.
    {
        ipc::file_mapping mapping(temp.path, ipc::read_only);
        bool threw = false;
        try {
            ipc::mapped_region region(mapping, ipc::read_only,
                                      static_cast<std::uint64_t>(original_data.size()), 0);
            (void)region;
        } catch (const std::system_error& error) {
            threw = true;
            expect_error_code(error, std::errc::invalid_argument, "zero-length mapping");
        }
        expect(threw, "zero-length mapping should throw");
    }

    // Copy-on-write mappings should see local modifications without altering the file.
    temp.write(original_data);
    {
        ipc::file_mapping mapping(temp.path, ipc::read_write);
        ipc::mapped_region region(mapping, ipc::copy_on_write, 0, 0);
        expect(region.size() == original_data.size(),
               "copy-on-write mapping should cover the entire file");
        auto* bytes = static_cast<std::uint8_t*>(region.data());
        expect(bytes != nullptr, "copy-on-write mapping returned null address");
        for (std::size_t i = 0; i < region.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(bytes[i] ^ 0xFFu);
        }
        for (std::size_t i = 0; i < region.size(); ++i) {
            if (bytes[i] == original_data[i]) {
                fail("copy-on-write mapping did not observe written data");
            }
        }
    }
    const auto after_copy_on_write = temp.read();
    for (std::size_t i = 0; i < after_copy_on_write.size(); ++i) {
        if (after_copy_on_write[i] != original_data[i]) {
            fail("copy-on-write mapping modified file contents");
        }
    }

    // Read-write mappings should persist modifications back to the file.
    temp.write(original_data);
    std::vector<std::uint8_t> expected = original_data;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<std::uint8_t>((expected[i] + 17u) & 0xFFu);
    }
    {
        ipc::file_mapping mapping(temp.path, ipc::read_write);
        ipc::mapped_region region(mapping, ipc::read_write, 0, 0);
        expect(region.size() == expected.size(),
               "read-write mapping should cover the entire file");
        auto* bytes = static_cast<std::uint8_t*>(region.data());
        expect(bytes != nullptr, "read-write mapping returned null address");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            bytes[i] = expected[i];
        }
    }
    const auto after_read_write = temp.read();
    for (std::size_t i = 0; i < after_read_write.size(); ++i) {
        if (after_read_write[i] != expected[i]) {
            fail("read-write mapping failed to persist modifications");
        }
    }

    return 0;
}

