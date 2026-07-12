// Verifies that Ring::attach() rejects a control file whose ABI fingerprint
// does not match this binary's compile-time fingerprint.
//
// Strategy: build a writer, close it (last detacher would normally remove the
// file, so we keep a second attached reference around to keep the file alive),
// then poke a wrong fingerprint into the on-disk control file. A new Ring_R
// constructed against the same name must throw ring_abi_mismatch_exception.

#include "sintra/detail/ipc/rings.h"
#include "sintra/detail/messaging/message.h"
#include "test_utils.h"

#include <cstddef>
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

void test_message_prefix_ring_abi()
{
    sintra::test::require_true(
        sintra::detail::k_sintra_ring_abi_version == 7,
        k_failure_prefix,
        "message-prefix layout requires ring ABI version 7");
    sintra::test::require_true(
        sintra::detail::k_ring_lifecycle_anchor_abi_version == 3,
        k_failure_prefix,
        "message-prefix layout must not change lifecycle-anchor ABI");
    sintra::test::require_true(
        sizeof(sintra::Message_prefix) == 64,
        k_failure_prefix,
        "supported message-prefix size must be 64 bytes");
    sintra::test::require_true(
        alignof(sintra::Message_prefix) == 8,
        k_failure_prefix,
        "supported message-prefix alignment must be 8 bytes");
    sintra::test::require_true(
        offsetof(sintra::Message_prefix, managed_child_custody_identity) == 48 &&
            offsetof(sintra::Message_prefix, managed_child_occurrence) == 56,
        k_failure_prefix,
        "managed-child metadata offsets must remain part of the versioned framing");

    const sintra::Message_prefix ordinary_prefix{};
    sintra::test::require_true(
        ordinary_prefix.managed_child_custody_identity == 0 &&
            ordinary_prefix.managed_child_occurrence == 0,
        k_failure_prefix,
        "ordinary message-prefix custody metadata must default to zero");
}

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

void write_fingerprint_prefix(const std::filesystem::path& path, std::uint64_t value)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    sintra::test::require_true(static_cast<bool>(f),
        k_failure_prefix,
        "could not create shared file for fingerprint test");
    f.write(reinterpret_cast<const char*>(&value), sizeof(value));
    sintra::test::require_true(static_cast<bool>(f),
        k_failure_prefix,
        "fingerprint prefix write failed");
}

void test_attach_rejects_mismatched_fingerprint()
{
    using element_t = std::uint32_t;

    const std::size_t capacity = sintra::aligned_capacity<element_t>(128);
    sintra::test::require_true(capacity != 0,
        k_failure_prefix,
        "aligned_capacity returned 0 for a valid request");

    const auto        scratch_dir = sintra::test::unique_scratch_directory("ring_abi_fingerprint");
    const std::string directory   = scratch_dir.string();
    const std::string ring_name   = "abi_fingerprint_ring";

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

    bool threw_typed   = false;
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

    // The failed Ring_R constructor must not leak its mapped control region.
    // Repeat the failed attach a few times; a leak would balloon virtual
    // memory or, on Windows, eventually exhaust mapping handles. We can't
    // assert the leak directly in a portable way, but the explicit retry
    // makes the failure mode (e.g. address-space exhaustion in ASan or
    // mapping-handle accumulation on Windows) reproducible if the leak
    // ever returns.
    for (int i = 0; i < 64; ++i) {
        bool retry_threw_typed = false;
        try {
            sintra::Ring_R<element_t> reader(directory, ring_name, capacity, 2);
            (void)reader;
        }
        catch (const sintra::ring_abi_mismatch_exception&) {
            retry_threw_typed = true;
        }
        sintra::test::require_true(retry_threw_typed,
            k_failure_prefix,
            "repeated mismatched attach should keep throwing the typed exception");
    }

    // Restore the correct fingerprint so the writer's destructor doesn't see
    // a corrupted control block during teardown.
    poke_fingerprint(control_file, sintra::detail::k_ring_abi_fingerprint);
}

void test_attach_rejects_mismatched_lifecycle_anchor()
{
    using element_t = std::uint32_t;

    const std::size_t capacity = sintra::aligned_capacity<element_t>(128);
    sintra::test::require_true(capacity != 0,
        k_failure_prefix,
        "aligned_capacity returned 0 for a valid lifecycle-anchor test request");

    const auto        scratch_dir = sintra::test::unique_scratch_directory("ring_lifecycle_anchor_abi");
    const std::string directory   = scratch_dir.string();
    const std::string ring_name   = "lifecycle_anchor_abi_ring";
    const auto        data_file   = scratch_dir / ring_name;
    const auto        control_file = scratch_dir / (ring_name + "_control");
    const auto        lifecycle_file = scratch_dir / (ring_name + "_lifecycle");

    constexpr std::uint64_t k_old_anchor_fingerprint = 0x73696e7472610001ull;
    write_fingerprint_prefix(lifecycle_file, k_old_anchor_fingerprint);

    bool threw_typed   = false;
    bool threw_generic = false;
    try {
        sintra::Ring_W<element_t> writer(directory, ring_name, capacity);
        (void)writer;
    }
    catch (const sintra::ring_abi_mismatch_exception& e) {
        threw_typed = true;
        sintra::test::require_true(
            e.observed_fingerprint() == k_old_anchor_fingerprint,
            k_failure_prefix,
            "lifecycle anchor exception should carry the observed fingerprint");
        sintra::test::require_true(
            e.expected_fingerprint() == sintra::detail::k_ring_lifecycle_anchor_fingerprint,
            k_failure_prefix,
            "lifecycle anchor exception should carry this binary's expected fingerprint");
    }
    catch (const std::exception&) {
        threw_generic = true;
    }

    sintra::test::require_true(threw_typed,
        k_failure_prefix,
        "attach should throw ring_abi_mismatch_exception on lifecycle anchor mismatch");
    sintra::test::require_true(!threw_generic,
        k_failure_prefix,
        "attach should not fall back to a generic exception on lifecycle anchor mismatch");
    sintra::test::require_true(!std::filesystem::exists(data_file),
        k_failure_prefix,
        "data file should not be created after lifecycle anchor ABI mismatch");
    sintra::test::require_true(!std::filesystem::exists(control_file),
        k_failure_prefix,
        "control file should not be created after lifecycle anchor ABI mismatch");
}

} // namespace

int main()
{
    try {
        test_message_prefix_ring_abi();
        test_attach_rejects_mismatched_fingerprint();
        test_attach_rejects_mismatched_lifecycle_anchor();
    }
    catch (const std::exception& ex) {
        std::cerr << "ring_abi_fingerprint_test failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
