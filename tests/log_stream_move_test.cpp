//
// sintra::Log_stream Move Semantics Test
//
// This test validates the move constructor and move assignment operator
// for Log_stream that was introduced in commit 392597d.
//
// The test verifies:
// - Move constructor transfers ownership and disables source
// - Move assignment transfers ownership and disables source
// - Moved-from stream does not output on destruction
// - Moved-to stream outputs correctly on destruction
// - Self-move assignment is handled safely
// - Real moves from factory functions (avoiding NRVO elision)
// - Postfix is preserved after move
//

#include <sintra/detail/logging.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

std::mutex g_log_mutex;
std::vector<std::string> g_captured_logs;
std::atomic<int> g_callback_count{0};

void test_log_callback(sintra::log_level /*level*/, const char* message, void* /*user_data*/)
{
    if (message) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_captured_logs.emplace_back(message);
        g_callback_count.fetch_add(1);
    }
}

void reset_captured_logs()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_captured_logs.clear();
    g_callback_count.store(0);
}

bool logs_contain(const std::string& substr)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    for (const auto& log : g_captured_logs) {
        if (log.find(substr) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int count_logs_containing(const std::string& substr)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    int count = 0;
    for (const auto& log : g_captured_logs) {
        if (log.find(substr) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

int get_log_count()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return static_cast<int>(g_captured_logs.size());
}

int test_move_constructor()
{
    reset_captured_logs();

    {
        sintra::Log_stream source(sintra::log_level::info);
        source << "move_ctor_test";

        // Move construct destination from source
        sintra::Log_stream destination(std::move(source));
        destination << "_additional";

        // Both go out of scope here, but only destination should output
    }

    // Should have exactly one log entry with the combined message
    if (!logs_contain("move_ctor_test_additional")) {
        std::fprintf(stderr, "Move constructor: expected combined message not found\n");
        return 1;
    }

    // Should only have one message (source was disabled by move)
    if (count_logs_containing("move_ctor_test") != 1) {
        std::fprintf(stderr, "Move constructor: expected exactly 1 log entry, got %d\n",
                     count_logs_containing("move_ctor_test"));
        return 1;
    }

    return 0;
}

int test_move_assignment()
{
    reset_captured_logs();

    {
        sintra::Log_stream source(sintra::log_level::info);
        source << "move_assign_source";

        sintra::Log_stream destination(sintra::log_level::info);
        destination << "move_assign_dest";

        // Move assign source to destination
        // This should transfer source's content to destination and disable source
        destination = std::move(source);
        destination << "_appended";
    }

    // Should have the source's message (now in destination) with appended text
    if (!logs_contain("move_assign_source_appended")) {
        std::fprintf(stderr, "Move assignment: expected source message with appended text\n");
        return 1;
    }

    if (logs_contain("move_assign_dest")) {
        std::fprintf(stderr, "Move assignment: destination content should not be logged\n");
        return 1;
    }

    if (count_logs_containing("move_assign_source") != 1) {
        std::fprintf(stderr, "Move assignment: expected exactly 1 log entry, got %d\n",
                     count_logs_containing("move_assign_source"));
        return 1;
    }

    return 0;
}

int test_move_from_factory_forced()
{
    reset_captured_logs();

    {
        // Force a real move by using an intermediate variable and std::move
        sintra::Log_stream temp = sintra::ls_info();
        temp << "forced_factory_move";

        // Explicitly move from temp to stream
        sintra::Log_stream stream(std::move(temp));
        stream << "_extended";

        // temp is now moved-from, should not output
        // stream should output
    }

    if (!logs_contain("forced_factory_move_extended")) {
        std::fprintf(stderr, "Forced factory move: expected message not found\n");
        return 1;
    }

    // Ensure only one log entry (temp was disabled)
    if (count_logs_containing("forced_factory_move") != 1) {
        std::fprintf(stderr, "Forced factory move: expected exactly 1 log entry\n");
        return 1;
    }

    return 0;
}

int test_self_move_assignment()
{
    reset_captured_logs();

    {
        sintra::Log_stream stream(sintra::log_level::info);
        stream << "self_move_content";

        // Self-move assignment - this should be handled safely
        // After self-move, the stream should either:
        // 1. Still work (if self-move is a no-op)
        // 2. Be disabled (if self-move clears the stream)
        // Either behavior is acceptable as long as it doesn't crash

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#endif
        stream = std::move(stream);
#ifdef __clang__
#pragma clang diagnostic pop
#endif

        // Try to append after self-move
        stream << "_after_self_move";
    }

    // The key test is that we didn't crash
    // The content may or may not be logged depending on implementation
    std::fprintf(stderr, "Self-move assignment: completed without crash, log count=%d\n",
                 get_log_count());

    return 0;
}

int test_disabled_stream_no_output()
{
    reset_captured_logs();

    {
        sintra::Log_stream stream(sintra::log_level::info, false); // disabled
        stream << "disabled_stream_test";
    }

    if (logs_contain("disabled_stream_test")) {
        std::fprintf(stderr, "Disabled stream: message should not have been logged\n");
        return 1;
    }

    return 0;
}

int test_moved_from_stream_disabled()
{
    reset_captured_logs();

    sintra::Log_stream source(sintra::log_level::info);
    source << "source_content";

    // Move the stream
    sintra::Log_stream destination(std::move(source));

    // Verify source can still be used (but does nothing)
    source << "_should_not_appear";

    // Destroy both explicitly by scope exit
    {
        // Move destination to ensure we test the moved-to stream's output
        sintra::Log_stream final_dest(std::move(destination));
    }

    if (logs_contain("_should_not_appear")) {
        std::fprintf(stderr, "Moved-from stream should not output\n");
        return 1;
    }

    if (!logs_contain("source_content")) {
        std::fprintf(stderr, "Destination should contain source content\n");
        return 1;
    }

    return 0;
}

int test_postfix_preserved()
{
    reset_captured_logs();

    {
        sintra::Log_stream source(sintra::log_level::info, true, "_postfix");
        source << "postfix_test";

        sintra::Log_stream destination(std::move(source));
    }

    if (!logs_contain("postfix_test_postfix")) {
        std::fprintf(stderr, "Postfix should be preserved after move\n");
        return 1;
    }

    return 0;
}

int test_chain_of_moves()
{
    reset_captured_logs();

    {
        sintra::Log_stream s1(sintra::log_level::info);
        s1 << "chain";

        sintra::Log_stream s2(std::move(s1));
        s2 << "_of";

        sintra::Log_stream s3(std::move(s2));
        s3 << "_moves";

        // Only s3 should output when it goes out of scope
    }

    if (!logs_contain("chain_of_moves")) {
        std::fprintf(stderr, "Chain of moves: expected combined message\n");
        return 1;
    }

    if (count_logs_containing("chain") != 1) {
        std::fprintf(stderr, "Chain of moves: expected exactly 1 log entry\n");
        return 1;
    }

    return 0;
}

int test_move_assignment_to_active_stream()
{
    reset_captured_logs();

    {
        sintra::Log_stream active(sintra::log_level::info);
        active << "will_be_replaced";

        sintra::Log_stream source(sintra::log_level::info);
        source << "replacement";

        // Move assign - active's content is replaced by source's
        active = std::move(source);
        active << "_content";
    }

    // Should have "replacement_content"
    if (!logs_contain("replacement_content")) {
        std::fprintf(stderr, "Move to active: expected replacement_content\n");
        return 1;
    }

    if (logs_contain("will_be_replaced")) {
        std::fprintf(stderr, "Move to active: original content should not be logged\n");
        return 1;
    }

    return 0;
}

} // namespace

int main()
{
    // Install our test callback
    sintra::set_log_callback(test_log_callback);

    int result = 0;

    result = test_move_constructor();
    if (result != 0) {
        std::fprintf(stderr, "test_move_constructor failed\n");
        return result;
    }
    std::fprintf(stderr, "test_move_constructor passed\n");

    result = test_move_assignment();
    if (result != 0) {
        std::fprintf(stderr, "test_move_assignment failed\n");
        return result;
    }
    std::fprintf(stderr, "test_move_assignment passed\n");

    result = test_move_from_factory_forced();
    if (result != 0) {
        std::fprintf(stderr, "test_move_from_factory_forced failed\n");
        return result;
    }
    std::fprintf(stderr, "test_move_from_factory_forced passed\n");

    result = test_self_move_assignment();
    if (result != 0) {
        std::fprintf(stderr, "test_self_move_assignment failed\n");
        return result;
    }
    std::fprintf(stderr, "test_self_move_assignment passed\n");

    result = test_disabled_stream_no_output();
    if (result != 0) {
        std::fprintf(stderr, "test_disabled_stream_no_output failed\n");
        return result;
    }
    std::fprintf(stderr, "test_disabled_stream_no_output passed\n");

    result = test_moved_from_stream_disabled();
    if (result != 0) {
        std::fprintf(stderr, "test_moved_from_stream_disabled failed\n");
        return result;
    }
    std::fprintf(stderr, "test_moved_from_stream_disabled passed\n");

    result = test_postfix_preserved();
    if (result != 0) {
        std::fprintf(stderr, "test_postfix_preserved failed\n");
        return result;
    }
    std::fprintf(stderr, "test_postfix_preserved passed\n");

    result = test_chain_of_moves();
    if (result != 0) {
        std::fprintf(stderr, "test_chain_of_moves failed\n");
        return result;
    }
    std::fprintf(stderr, "test_chain_of_moves passed\n");

    result = test_move_assignment_to_active_stream();
    if (result != 0) {
        std::fprintf(stderr, "test_move_assignment_to_active_stream failed\n");
        return result;
    }
    std::fprintf(stderr, "test_move_assignment_to_active_stream passed\n");

    // Restore default callback
    sintra::set_log_callback(nullptr);

    std::fprintf(stderr, "All Log_stream move tests passed\n");
    return 0;
}
