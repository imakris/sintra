#include <array>
#include <cstdint>
#include <cstdio>
#include <new>
#include <vector>

#include "sintra/detail/process_message_reader.h"

#include "test_environment.h"

namespace
{

[[noreturn]] void fail(const char* message)
{
    std::fprintf(stderr, "delivery_fence_reply_skip_test failure: %s\n", message);
    std::exit(1);
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        fail(message);
    }
}

using Progress = sintra::Process_message_reader::Delivery_progress;

struct Progress_storage
{
    alignas(Progress) std::array<unsigned char, sizeof(Progress)> storage{};

    Progress* construct()
    {
        return new (storage.data()) Progress();
    }

    void destroy(Progress* progress)
    {
        if (progress) {
            progress->~Progress();
        }
    }
};

} // namespace

int main()
{
    Progress_storage storage{};

    // Stage 1: register a skip for the initial progress object.
    Progress* first = storage.construct();
    const auto first_generation = first->generation;
    first->reply_sequence.store(32, std::memory_order_release);

    sintra::register_reply_progress_skip(first, first_generation, 40, 30);

    // Destroy the original progress object and construct a new one at the same address.
    storage.destroy(first);
    Progress* second = storage.construct();
    const auto second_generation = second->generation;
    second->reply_sequence.store(64, std::memory_order_release);

    auto skips = sintra::take_reply_progress_skips();
    expect(skips.size() == 1, "expected one skip entry after takeover");

    // Applying the skip against the new generation must be ignored.
    const bool consumed_mismatch = sintra::detail::consume_reply_progress_skip(
        skips,
        second,
        second_generation,
        48,
        30);
    expect(!consumed_mismatch, "generation mismatch should not consume skip");
    expect(skips.empty(), "skip vector should be cleared after mismatch");

    // Stage 2: register a skip for the active progress object and ensure it is consumed.
    sintra::register_reply_progress_skip(second, second_generation, 72, 60);
    auto skips_again = sintra::take_reply_progress_skips();
    expect(skips_again.size() == 1, "expected skip entry for current generation");

    second->reply_sequence.store(80, std::memory_order_release);
    const bool consumed_match = sintra::detail::consume_reply_progress_skip(
        skips_again,
        second,
        second_generation,
        72,
        60);
    expect(consumed_match, "matching generation should consume skip");
    expect(skips_again.empty(), "skip vector should be empty after successful consumption");

    storage.destroy(second);
    return 0;
}
