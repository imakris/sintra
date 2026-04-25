// Verifies that Ring::attach() rejects a control file whose ABI fingerprint
// does not match this binary's compile-time fingerprint.
//
// Strategy: build a writer, close it (last detacher would normally remove the
// file, so we keep a second attached reference around to keep the file alive),
// then poke a wrong fingerprint into the on-disk control file. A new Ring_R
// constructed against the same name must throw ring_abi_mismatch_exception.

#include "sintra/detail/ipc/rings.h"
#include "test_utils.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view k_failure_prefix = "ring_abi_fingerprint_test: ";

void poke_fingerprint(const std::filesystem::path& control_file, std::uint64_t value)
{
    std::fstream f(control_file, std::ios::binary | std::ios::in | std::ios::out);
    sintra::test::require_true(static_cast<bool>(f),
                               k_failure_prefix,
                               "could not open control file for fingerprint poke");
    // abi_fingerprint is the first member of Control, so it sits at offset 0.
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&value), sizeof(value));
    sintra::test::require_true(static_cast<bool>(f),
                               k_failure_prefix,
                               "fingerprint poke write failed");
    f.flush();
}

void test_attach_rejects_mismatched_fingerprint()
{
    using element_t = std::uint32_t;

    const std::size_t capacity = sintra::aligned_capacity<element_t>(128);
    sintra::test::require_true(capacity != 0,
                               k_failure_prefix,
                               "aligned_capacity returned 0 for a valid request");

    const auto scratch_dir = sintra::test::unique_scratch_directory("ring_abi_fingerprint");
    const std::string directory = scratch_dir.string();
    const std::string ring_name = "abi_fingerprint_ring";

    // First: a writer creates the control file with the correct fingerprint.
    // We keep this writer alive while we poke a wrong fingerprint into the
    // file and try to attach a second Ring; this prevents the control file
    // from being torn down by the last detacher between the two phases.
    sintra::Ring_W<element_t> writer(directory, ring_name, capacity);

    const auto control_file = scratch_dir / (ring_name + "_control");
    sintra::test::require_true(std::filesystem::exists(control_file),
                               k_failure_prefix,
                               "control file should exist after writer construction");

    // Sanity: a second attach against the same name must succeed while the
    // fingerprint is intact, otherwise the test is ill-formed.
    {
        sintra::Ring_R<element_t> reader(directory, ring_name, capacity, 2);
        (void)reader;
    }

    // Corrupt the fingerprint and confirm a fresh attach throws.
    constexpr std::uint64_t k_wrong_fingerprint = 0xdeadbeefcafef00dull;
    poke_fingerprint(control_file, k_wrong_fingerprint);

    bool threw_typed = false;
    bool threw_generic = false;
    try {
        sintra::Ring_R<element_t> reader(directory, ring_name, capacity, 2);
        (void)reader;
    }
    catch (const sintra::ring_abi_mismatch_exception& e) {
        threw_typed = true;
        sintra::test::require_true(
            e.observed_fingerprint() == k_wrong_fingerprint,
            k_failure_prefix,
            "exception should carry the observed fingerprint");
        sintra::test::require_true(
            e.expected_fingerprint() == sintra::detail::k_ring_abi_fingerprint,
            k_failure_prefix,
            "exception should carry this binary's expected fingerprint");
    }
    catch (const std::exception&) {
        threw_generic = true;
    }

    sintra::test::require_true(threw_typed,
                               k_failure_prefix,
                               "attach should throw ring_abi_mismatch_exception on fingerprint mismatch");
    sintra::test::require_true(!threw_generic,
                               k_failure_prefix,
                               "attach should not fall back to a generic exception on fingerprint mismatch");

    // Restore the correct fingerprint so the writer's destructor doesn't see
    // a corrupted control block during teardown.
    poke_fingerprint(control_file, sintra::detail::k_ring_abi_fingerprint);
}

} // namespace

int main()
{
    try {
        test_attach_rejects_mismatched_fingerprint();
    }
    catch (const std::exception& ex) {
        std::cerr << "ring_abi_fingerprint_test failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
