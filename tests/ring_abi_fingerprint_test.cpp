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

#include <atomic>
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
        sintra::detail::k_sintra_ring_abi_version == 10,
        k_failure_prefix,
        "reader copying marks and process identity require ring ABI version 10");
    sintra::test::require_true(
        sintra::detail::k_ring_lifecycle_anchor_abi_version == 5,
        k_failure_prefix,
        "process-instance mutex owners require lifecycle-anchor ABI version 5");
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
    sintra::test::require_true(sintra::detail::write_private_file(path,
        std::string(reinterpret_cast<const char*>(&value), sizeof(value))),
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

    // Controls from the previous shared-copy-lock protocol (ABI 9), from the
    // first ABI-10 layout, whose reader slots lacked namespace identities, from
    // ABI-10 revision 2, whose Windows writers rebuild ownership_mutex from
    // writer_pid, from revision 3, whose locks recorded owners by PID alone,
    // and from revision 4, whose second spinlock word was a wall-clock stamp,
    // must be rejected even when their size happens to match.
    const auto previous_fingerprint = [](std::uint64_t abi_version) {
        return sintra::detail::fnv1a_64({
            abi_version,
            static_cast<uint64_t>(sintra::num_process_index_bits),
            static_cast<uint64_t>(sintra::max_process_index),
            static_cast<uint64_t>(sintra::max_message_length),
            static_cast<uint64_t>(sintra::assumed_cache_line_size),
            static_cast<uint64_t>(sintra::num_reserved_service_instances),
        });
    };
    const auto revision_fingerprint = [](std::uint64_t revision) {
        return sintra::detail::fnv1a_64({
            sintra::detail::k_sintra_ring_abi_version,
            revision,
            static_cast<uint64_t>(sintra::num_process_index_bits),
            static_cast<uint64_t>(sintra::max_process_index),
            static_cast<uint64_t>(sintra::max_message_length),
            static_cast<uint64_t>(sintra::assumed_cache_line_size),
            static_cast<uint64_t>(sintra::num_reserved_service_instances),
        });
    };
    sintra::test::require_true(sintra::detail::k_ring_abi_layout_revision == 5,
        k_failure_prefix,
        "spinlock generation words require ABI-10 layout revision 5");
    sintra::test::require_true(
        revision_fingerprint(5) == sintra::detail::k_ring_abi_fingerprint,
        k_failure_prefix,
        "the revision fixture must reproduce this build's fingerprint");

    for (const std::uint64_t wrong_fingerprint :
            {previous_fingerprint(9), previous_fingerprint(10), revision_fingerprint(2),
                revision_fingerprint(3), revision_fingerprint(4)})
    {
        sintra::test::require_true(wrong_fingerprint != sintra::detail::k_ring_abi_fingerprint,
            k_failure_prefix,
            "an earlier control layout must not share this build's fingerprint");
        poke_fingerprint(control_file, wrong_fingerprint);

        bool threw_typed   = false;
        bool threw_generic = false;
        try {
            sintra::Ring_R<element_t> reader(directory, ring_name, capacity, 2);
            (void)reader;
        }
        catch (const sintra::ring_abi_mismatch_exception& e) {
            threw_typed = true;
            sintra::test::require_true(
                e.observed_fingerprint() == wrong_fingerprint,
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

// A persistent anchor left by an ABI-4 build has this build's size, but its
// mutex records owners by PID alone. Its fingerprint must reject it before any
// locking or cleanup.
void test_attach_rejects_previous_lifecycle_anchor()
{
    using element_t = std::uint32_t;
    namespace detail = sintra::detail;

    const std::size_t capacity       = sintra::aligned_capacity<element_t>(128);
    const auto        scratch_dir    = sintra::test::unique_scratch_directory("ring_lifecycle_anchor_abi4");
    const std::string directory      = scratch_dir.string();
    const std::string ring_name      = "lifecycle_anchor_abi4_ring";
    const auto        data_file      = scratch_dir / ring_name;
    const auto        control_file   = scratch_dir / (ring_name + "_control");
    const auto        lifecycle_file = scratch_dir / (ring_name + "_lifecycle");

    {
        sintra::Ring_W<element_t> writer(directory, ring_name, capacity);
    }
    sintra::test::require_true(
        std::filesystem::exists(lifecycle_file) &&
            !std::filesystem::exists(data_file) && !std::filesystem::exists(control_file),
        k_failure_prefix,
        "the last detach should remove the ring files and keep the lifecycle anchor");

    const std::uint64_t abi4_fingerprint = detail::fnv1a_64({
        0x73696e7472615f6cull,
        4,
        4,
        static_cast<uint64_t>(sizeof(std::atomic<uint64_t>)),
        static_cast<uint64_t>(alignof(std::atomic<uint64_t>)),
        static_cast<uint64_t>(sizeof(detail::interprocess_mutex)),
        static_cast<uint64_t>(alignof(detail::interprocess_mutex)),
        static_cast<uint64_t>(sizeof(std::atomic<uint32_t>)),
        static_cast<uint64_t>(alignof(std::atomic<uint32_t>)),
        static_cast<uint64_t>(detail::k_ring_lifecycle_attachment_slots),
        static_cast<uint64_t>(sizeof(detail::ring_lifecycle_attachment_record)),
        static_cast<uint64_t>(alignof(detail::ring_lifecycle_attachment_record)),
    });
    sintra::test::require_true(abi4_fingerprint != detail::k_ring_lifecycle_anchor_fingerprint,
        k_failure_prefix,
        "an ABI-4 lifecycle anchor must not share this build's fingerprint");
    poke_fingerprint(lifecycle_file, abi4_fingerprint);

    bool threw_typed = false;
    try {
        sintra::Ring_W<element_t> writer(directory, ring_name, capacity);
        (void)writer;
    }
    catch (const sintra::ring_abi_mismatch_exception& e) {
        threw_typed = e.observed_fingerprint() == abi4_fingerprint &&
            e.expected_fingerprint() == detail::k_ring_lifecycle_anchor_fingerprint;
    }
    sintra::test::require_true(threw_typed,
        k_failure_prefix,
        "attach should reject an ABI-4 lifecycle anchor with the typed exception");
    sintra::test::require_true(
        !std::filesystem::exists(data_file) && !std::filesystem::exists(control_file),
        k_failure_prefix,
        "ring files should not be created after an ABI-4 lifecycle anchor is rejected");
}

} // namespace

int main()
{
    try {
        test_message_prefix_ring_abi();
        test_attach_rejects_mismatched_fingerprint();
        test_attach_rejects_mismatched_lifecycle_anchor();
        test_attach_rejects_previous_lifecycle_anchor();
    }
    catch (const std::exception& ex) {
        std::cerr << "ring_abi_fingerprint_test failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
