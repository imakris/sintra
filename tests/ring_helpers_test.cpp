#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "sintra/detail/ipc/rings.h"
#include "test_environment.h"

namespace {

void require_true(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_aligned_capacity()
{
    using element_t = std::uint32_t;

    const std::size_t page_size = sintra::system_page_size();
    const std::size_t requested = 1;

    const std::size_t aligned = sintra::aligned_capacity<element_t>(requested);
    require_true(aligned >= requested, "aligned_capacity should not reduce the requested size");
    require_true(aligned % 8 == 0, "aligned_capacity must return a multiple of 8 elements");
    if (page_size != 0) {
        require_true((aligned * sizeof(element_t)) % page_size == 0,
            "aligned_capacity must respect page alignment");
    }

    require_true(sintra::aligned_capacity<element_t>(0) == 0,
        "aligned_capacity(0) should return 0");

    const std::size_t runtime_aligned = sintra::aligned_capacity(requested, sizeof(element_t));
    require_true(runtime_aligned == aligned, "aligned_capacity overloads should match");
}

void test_write_commit()
{
    using element_t = std::uint32_t;

    const std::size_t capacity = sintra::aligned_capacity<element_t>(128);
    require_true(capacity != 0, "aligned_capacity returned 0 for a valid request");

    const auto scratch_dir = sintra::test::unique_scratch_directory("ring_helpers");
    const std::string directory = scratch_dir.string();
    const std::string ring_name = "ring_helpers_ring";

    sintra::Ring_W<element_t> writer(directory, ring_name, capacity);
    sintra::Ring_R<element_t> reader(directory, ring_name, capacity, 2);

    const element_t first_value = 42;
    const auto seq1 = writer.write_commit(first_value);
    require_true(seq1 == 1, "write_commit should return the new leading sequence");

    auto range1 = reader.start_reading(1);
    require_true(range1.begin && range1.end, "start_reading returned an invalid range");
    require_true(range1.end - range1.begin == 1, "expected a single element in range");
    require_true(range1.begin[0] == first_value, "unexpected value after write_commit");
    reader.done_reading();

    const element_t batch[] = {10, 11};
    const auto seq2 = writer.write_commit(batch, 2);
    require_true(seq2 == 3, "write_commit should advance the leading sequence");

    auto range2 = reader.start_reading(2);
    require_true(range2.end - range2.begin == 2, "expected two elements in range");
    require_true(range2.begin[0] == batch[0], "first batch element mismatch");
    require_true(range2.begin[1] == batch[1], "second batch element mismatch");
    reader.done_reading();
}

} // namespace

int main()
{
    try {
        test_aligned_capacity();
        test_write_commit();
    }
    catch (const std::exception& ex) {
        std::cerr << "ring_helpers_test failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
