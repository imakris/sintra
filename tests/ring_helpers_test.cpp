#include "sintra/detail/ipc/rings.h"
#include "test_utils.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

constexpr std::string_view k_failure_prefix = "ring_helpers_test: ";

void test_aligned_capacity()
{
    using element_t = std::uint32_t;

    const std::size_t page_size = sintra::system_page_size();
    const std::size_t requested = 1;

    const std::size_t aligned = sintra::aligned_capacity<element_t>(requested);
    sintra::test::require_true(aligned >= requested,
                               k_failure_prefix,
                               "aligned_capacity should not reduce the requested size");
    sintra::test::require_true(aligned % 8 == 0,
                               k_failure_prefix,
                               "aligned_capacity must return a multiple of 8 elements");
    if (page_size != 0) {
        sintra::test::require_true((aligned * sizeof(element_t)) % page_size == 0,
                                   k_failure_prefix,
                                   "aligned_capacity must respect page alignment");
    }

    sintra::test::require_true(sintra::aligned_capacity<element_t>(0) == 0,
                               k_failure_prefix,
                               "aligned_capacity(0) should return 0");

    const std::size_t runtime_aligned = sintra::aligned_capacity(requested, sizeof(element_t));
    sintra::test::require_true(runtime_aligned == aligned,
                               k_failure_prefix,
                               "aligned_capacity overloads should match");
}

void test_write_commit()
{
    using element_t = std::uint32_t;

    const std::size_t capacity = sintra::aligned_capacity<element_t>(128);
    sintra::test::require_true(capacity != 0,
                               k_failure_prefix,
                               "aligned_capacity returned 0 for a valid request");

    const auto scratch_dir = sintra::test::unique_scratch_directory("ring_helpers");
    const std::string directory = scratch_dir.string();
    const std::string ring_name = "ring_helpers_ring";

    sintra::Ring_W<element_t> writer(directory, ring_name, capacity);
    sintra::Ring_R<element_t> reader(directory, ring_name, capacity, 2);

    const element_t first_value = 42;
    const auto seq1 = writer.write_commit(first_value);
    sintra::test::require_true(seq1 == 1,
                               k_failure_prefix,
                               "write_commit should return the new leading sequence");

    auto range1 = reader.start_reading(1);
    sintra::test::require_true(range1.begin && range1.end,
                               k_failure_prefix,
                               "start_reading returned an invalid range");
    sintra::test::require_true(range1.end - range1.begin == 1,
                               k_failure_prefix,
                               "expected a single element in range");
    sintra::test::require_true(range1.begin[0] == first_value,
                               k_failure_prefix,
                               "unexpected value after write_commit");
    reader.done_reading();

    const element_t batch[] = {10, 11};
    const auto seq2 = writer.write_commit(batch, 2);
    sintra::test::require_true(seq2 == 3,
                               k_failure_prefix,
                               "write_commit should advance the leading sequence");

    auto range2 = reader.start_reading(2);
    sintra::test::require_true(range2.end - range2.begin == 2,
                               k_failure_prefix,
                               "expected two elements in range");
    sintra::test::require_true(range2.begin[0] == batch[0],
                               k_failure_prefix,
                               "first batch element mismatch");
    sintra::test::require_true(range2.begin[1] == batch[1],
                               k_failure_prefix,
                               "second batch element mismatch");
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
