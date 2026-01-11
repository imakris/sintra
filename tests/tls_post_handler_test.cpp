//
// Sintra TLS Post Handler Test
//
// This test validates the thread-local post handler utilities introduced in
// commit 7481284 (tls_post_handler.h).
//
// The test verifies:
// - tl_post_handler_function_ref() creates and returns a reference to the TLS function
// - tl_post_handler_function_ready() returns correct status
// - tl_post_handler_function_clear() clears the function
// - tl_post_handler_function_release() releases the allocated memory
// - Thread-local isolation (each thread has its own handler)
//

#include <sintra/detail/tls_post_handler.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

std::atomic<int> g_handler_call_count{0};

void test_handler()
{
    g_handler_call_count.fetch_add(1);
}

int test_initial_state()
{
    // Initially, the TLS function should not be ready
    if (sintra::tl_post_handler_function_ready()) {
        std::fprintf(stderr, "Initial state: handler should not be ready\n");
        return 1;
    }

    return 0;
}

int test_ref_creates_function()
{
    // Getting the ref should create the function object
    auto& func = sintra::tl_post_handler_function_ref();

    // The function object exists but is empty, so not "ready"
    if (sintra::tl_post_handler_function_ready()) {
        std::fprintf(stderr, "After ref: empty function should not be ready\n");
        return 1;
    }

    // Assign a handler
    func = test_handler;

    // Now it should be ready
    if (!sintra::tl_post_handler_function_ready()) {
        std::fprintf(stderr, "After assignment: handler should be ready\n");
        return 1;
    }

    // Clean up
    sintra::tl_post_handler_function_release();
    return 0;
}

int test_handler_invocation()
{
    g_handler_call_count.store(0);

    auto& func = sintra::tl_post_handler_function_ref();
    func = test_handler;

    // Invoke the handler
    func();

    if (g_handler_call_count.load() != 1) {
        std::fprintf(stderr, "Handler invocation: expected 1 call, got %d\n",
                     g_handler_call_count.load());
        return 1;
    }

    // Invoke again
    func();

    if (g_handler_call_count.load() != 2) {
        std::fprintf(stderr, "Handler invocation: expected 2 calls, got %d\n",
                     g_handler_call_count.load());
        return 1;
    }

    // Clean up
    sintra::tl_post_handler_function_release();
    return 0;
}

int test_clear_function()
{
    auto& func = sintra::tl_post_handler_function_ref();
    func = test_handler;

    if (!sintra::tl_post_handler_function_ready()) {
        std::fprintf(stderr, "Before clear: handler should be ready\n");
        return 1;
    }

    // Clear the function
    sintra::tl_post_handler_function_clear();

    // Should no longer be ready
    if (sintra::tl_post_handler_function_ready()) {
        std::fprintf(stderr, "After clear: handler should not be ready\n");
        return 1;
    }

    // Clean up
    sintra::tl_post_handler_function_release();
    return 0;
}

int test_release_function()
{
    // Create the function
    auto& func = sintra::tl_post_handler_function_ref();
    func = test_handler;

    // Release it
    sintra::tl_post_handler_function_release();

    // After release, should not be ready
    if (sintra::tl_post_handler_function_ready()) {
        std::fprintf(stderr, "After release: handler should not be ready\n");
        return 1;
    }

    // Getting ref again should work (creates new allocation)
    auto& func2 = sintra::tl_post_handler_function_ref();
    func2 = test_handler;

    if (!sintra::tl_post_handler_function_ready()) {
        std::fprintf(stderr, "After re-ref: handler should be ready\n");
        return 1;
    }

    // Clean up
    sintra::tl_post_handler_function_release();
    return 0;
}

int test_thread_local_isolation()
{
    std::atomic<bool> thread1_ready{false};
    std::atomic<bool> thread2_ready{false};
    std::atomic<int> thread1_result{0};
    std::atomic<int> thread2_result{0};
    std::atomic<int> thread1_calls{0};
    std::atomic<int> thread2_calls{0};

    auto thread1_func = [&]() {
        thread1_calls.fetch_add(1);
    };

    auto thread2_func = [&]() {
        thread2_calls.fetch_add(1);
    };

    std::thread t1([&]() {
        auto& func = sintra::tl_post_handler_function_ref();
        func = thread1_func;
        thread1_ready.store(true);

        // Wait for thread2 to be ready
        while (!thread2_ready.load()) {
            std::this_thread::yield();
        }

        // Invoke our handler
        func();
        func();

        // Check our call count (should be 2, not affected by thread2)
        if (thread1_calls.load() != 2) {
            thread1_result.store(1);
        }

        sintra::tl_post_handler_function_release();
    });

    std::thread t2([&]() {
        auto& func = sintra::tl_post_handler_function_ref();
        func = thread2_func;
        thread2_ready.store(true);

        // Wait for thread1 to be ready
        while (!thread1_ready.load()) {
            std::this_thread::yield();
        }

        // Invoke our handler
        func();

        // Check our call count (should be 1, not affected by thread1)
        if (thread2_calls.load() != 1) {
            thread2_result.store(1);
        }

        sintra::tl_post_handler_function_release();
    });

    t1.join();
    t2.join();

    if (thread1_result.load() != 0) {
        std::fprintf(stderr, "Thread isolation: thread1 handler count wrong\n");
        return 1;
    }

    if (thread2_result.load() != 0) {
        std::fprintf(stderr, "Thread isolation: thread2 handler count wrong\n");
        return 1;
    }

    // Verify total counts
    if (thread1_calls.load() != 2) {
        std::fprintf(stderr, "Thread isolation: expected thread1_calls=2, got %d\n",
                     thread1_calls.load());
        return 1;
    }

    if (thread2_calls.load() != 1) {
        std::fprintf(stderr, "Thread isolation: expected thread2_calls=1, got %d\n",
                     thread2_calls.load());
        return 1;
    }

    return 0;
}

int test_double_release()
{
    // Create and release
    auto& func = sintra::tl_post_handler_function_ref();
    func = test_handler;
    sintra::tl_post_handler_function_release();

    // Second release should be safe (no-op)
    sintra::tl_post_handler_function_release();

    return 0;
}

int test_clear_without_allocation()
{
    // Clear without prior allocation should be safe (pointer is null)
    // Note: We need to release any existing allocation first
    sintra::tl_post_handler_function_release();

    // This should be a no-op since tl_post_handler_function is null
    sintra::tl_post_handler_function_clear();

    return 0;
}

} // namespace

int main()
{
    int result = 0;

    result = test_initial_state();
    if (result != 0) {
        std::fprintf(stderr, "test_initial_state failed\n");
        return result;
    }
    std::fprintf(stderr, "test_initial_state passed\n");

    result = test_ref_creates_function();
    if (result != 0) {
        std::fprintf(stderr, "test_ref_creates_function failed\n");
        return result;
    }
    std::fprintf(stderr, "test_ref_creates_function passed\n");

    result = test_handler_invocation();
    if (result != 0) {
        std::fprintf(stderr, "test_handler_invocation failed\n");
        return result;
    }
    std::fprintf(stderr, "test_handler_invocation passed\n");

    result = test_clear_function();
    if (result != 0) {
        std::fprintf(stderr, "test_clear_function failed\n");
        return result;
    }
    std::fprintf(stderr, "test_clear_function passed\n");

    result = test_release_function();
    if (result != 0) {
        std::fprintf(stderr, "test_release_function failed\n");
        return result;
    }
    std::fprintf(stderr, "test_release_function passed\n");

    result = test_thread_local_isolation();
    if (result != 0) {
        std::fprintf(stderr, "test_thread_local_isolation failed\n");
        return result;
    }
    std::fprintf(stderr, "test_thread_local_isolation passed\n");

    result = test_double_release();
    if (result != 0) {
        std::fprintf(stderr, "test_double_release failed\n");
        return result;
    }
    std::fprintf(stderr, "test_double_release passed\n");

    result = test_clear_without_allocation();
    if (result != 0) {
        std::fprintf(stderr, "test_clear_without_allocation failed\n");
        return result;
    }
    std::fprintf(stderr, "test_clear_without_allocation passed\n");

    std::fprintf(stderr, "All TLS post handler tests passed\n");
    return 0;
}
