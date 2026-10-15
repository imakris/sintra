#include "isolated_test_support.h"

#include <sintra/detail/ipc/mutex.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace mutex_cleanup {

using sintra::test::isolated::await;
using sintra::test::isolated::require;
using sintra::test::isolated::case_t;

constexpr case_t k_cases[] = {
#if !defined(_WIN32)
    {"mandatory_acquisition_real_error", "", "acquisition_error"},
    {"native_backoff_interrupted", "", "backoff_error"},
    {"native_backoff_genuine_error", "", "backoff_error"},
#endif
    {"mutex_timed_before_cas", "mutex_before_owner_cas", "timed_throw"},
    {"mutex_timed_after_cas", "mutex_after_owner_cas", "timed_throw"},
    {"mutex_acquire_throw_cleanup", "mutex_before_owner_cas", "gate_throw"},
    {"mutex_recovery_throw_cleanup", "mutex_recovery_gate_acquired", "gate_throw"},
    {"mutex_mixed_recursion", "", "recursion"},
};

struct Recursion_probe
{
    const void* mutex;
    std::atomic<bool> snapshot_loaded{false};
    std::atomic<bool> other_owns{false};
    std::atomic<bool> gate_held{false};
    std::atomic<bool> attempted{false};
};
Recursion_probe* s_recursion_probe = nullptr;

int run_recursion_case()
{
    using Mutex = sintra::detail::interprocess_mutex;
    const auto instance = sintra::detail::current_process_instance();
    const auto pid = uint32_t(sintra::get_current_pid());
    const auto tid = uint32_t(sintra::get_current_tid());
    const auto earlier_token = uint32_t(instance) + 1u;
    Mutex mutex;
    mutex.test_install_owner_fixture({pid, tid, 0, earlier_token});
    const auto stale_owner = mutex.test_owner_token();
    Recursion_probe probe{&mutex};
    s_recursion_probe = &probe;
    sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void* object) {
        if (object == s_recursion_probe->mutex &&
            std::string_view(stage) == "mutex_recovery_gate_acquired")
        {
            s_recursion_probe->gate_held = true;
            require(await([] {
                return s_recursion_probe->snapshot_loaded.load() || s_recursion_probe->attempted.load();
            }), "contender did not enter the recursion decision");
        }
        if (object == s_recursion_probe->mutex &&
            std::string_view(stage) == "mutex_recursion_after_owner_load")
        {
            s_recursion_probe->snapshot_loaded = true;
            require(await([] { return s_recursion_probe->other_owns.load(); }),
                "other thread did not acquire after the stale recursion snapshot");
        }
    };
    std::atomic<bool> release{false};
    std::thread other([&] {
        mutex.lock();
        probe.other_owns = true;
        require(await([&] { return release.load(); }), "contender did not release the fixture");
        mutex.unlock();
    });
    require(await([&] { return probe.gate_held.load(); }), "recovery gate was not held by the other thread");
    bool false_recursion = false;
    bool acquired = false;
    try { acquired = mutex.test_try_lock_throwing(); }
    catch (const std::system_error& error) {
        require(error.code() == std::make_error_code(std::errc::resource_deadlock_would_occur),
            "recursion attempt threw an unrelated error");
        false_recursion = true;
    }
    probe.attempted = true;
    require(await([&] { return probe.other_owns.load(); }), "successor thread did not acquire the mutex");
    const auto current_owner = mutex.test_owner_token();
    release = true;
    other.join();
    sintra::detail::test_hooks::s_mutex_operation = nullptr;
    s_recursion_probe = nullptr;
    require(!acquired && current_owner != stale_owner,
        "recursion fixture did not establish a different actual owner");
    require(!false_recursion,
        "mixed ownership generations produced false recursion while another thread owned mutex");
    return 0;
}

const void* s_throw_mutex = nullptr;
const char* s_throw_mutex_stage = nullptr;

int run_gate_exception_case(const case_t& test)
{
    sintra::detail::interprocess_mutex mutex;
    if (std::string_view(test.name) == "mutex_recovery_throw_cleanup") {
        mutex.test_install_owner_fixture({0, 1, 0, 0});
    }
    s_throw_mutex = &mutex;
    s_throw_mutex_stage = test.poster_stage;
    sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void* object) {
        if (object == s_throw_mutex && std::string_view(stage) == s_throw_mutex_stage) {
            throw std::runtime_error("fixture gate hook exception");
        }
    };
    bool caught = false;
    try { (void)mutex.test_try_lock_throwing(); }
    catch (const std::runtime_error&) { caught = true; }
    sintra::detail::test_hooks::s_mutex_operation = nullptr;
    require(caught, "gate exception fixture did not reach its throwing hook");
    const auto holder = mutex.test_gate_holder();
    const bool acquired = mutex.try_lock();
    if (acquired) { mutex.unlock(); }
    require(holder == 0 && acquired,
        "throwing infrastructure hook retained live recovery gate and blocked the next acquisition");
    return 0;
}

sintra::detail::interprocess_mutex* s_timed_mutex = nullptr;
std::atomic<bool> s_probe_thrown{false};

void timed_termination()
{
    std::fprintf(stderr, "%s: timed noexcept termination; gate=%llu self=%llu owner=%llu\n",
        s_throw_mutex_stage,
        static_cast<unsigned long long>(s_timed_mutex->test_gate_holder()),
        static_cast<unsigned long long>(sintra::detail::current_process_instance()),
        static_cast<unsigned long long>(s_timed_mutex->test_owner_token()));
    std::fflush(stderr);
    std::_Exit(79);
}

int run_timed_exception_case(const case_t& test)
{
    sintra::detail::interprocess_mutex mutex;
    s_timed_mutex = &mutex;
    s_throw_mutex_stage = test.poster_stage;
    std::set_terminate(timed_termination);
    sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void* object) {
        if (object == s_timed_mutex && std::string_view(stage) == s_throw_mutex_stage) {
            s_probe_thrown = true;
            throw std::runtime_error("fixture timed acquisition exception");
        }
    };
    const bool acquired = mutex.try_lock_until(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    sintra::detail::test_hooks::s_mutex_operation = nullptr;
    const bool after_cas = std::string_view(test.poster_stage) == "mutex_after_owner_cas";
    require(s_probe_thrown && acquired == after_cas && mutex.test_gate_holder() == 0,
        "timed containment lost this call's successful CAS outcome or gate cleanup");
    require(sintra::detail::test_hooks::take_observation_failure() != nullptr,
        "timed acquisition swallowed the deferred instrumentation failure");
    if (acquired) { mutex.unlock(); }
    require(mutex.try_lock(), "timed exception left ownership unavailable");
    mutex.unlock();
    return 0;
}

#if !defined(_WIN32)
int run_mandatory_error_case()
{
    int channel[2];
    require(::pipe(channel) == 0, "cannot capture direct acquisition diagnostics");
    const int original_stderr = ::dup(STDERR_FILENO);
    require(original_stderr >= 0 && ::dup2(channel[1], STDERR_FILENO) >= 0,
        "cannot redirect native diagnostic output");
    ::close(channel[1]);
    require(::fcntl(channel[0], F_SETFL, O_NONBLOCK) == 0, "cannot read diagnostic capture without blocking");
    sintra::detail::interprocess_mutex mutex;
    sintra::detail::test_hooks::s_process_instance_fork_error = EAGAIN;
    std::atomic<bool> acquired{false};
    uint64_t token = 0;
    std::thread worker([&] { mutex.lock_cleanup(token); acquired = true; });
    std::string observed;
    const auto drain = [&] {
        char bytes[1024];
        const auto count = ::read(channel[0], bytes, sizeof(bytes));
        if (count > 0) { observed.append(bytes, size_t(count)); }
    };
    const auto wait_for_error = [&](int error) {
        const std::string expected = "mandatory mutex acquisition error " + std::to_string(error) + ";";
        // Complete the negative oracle, clear the real error and join before
        // the outer eight-second child supervisor can expire.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        do {
            drain();
            if (observed.find(expected) != std::string::npos) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    const bool first_visible = wait_for_error(EAGAIN);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    drain();
    const auto first_bytes = observed.size();
    sintra::detail::test_hooks::s_process_instance_fork_error = ENOMEM;
    const bool change_visible = wait_for_error(ENOMEM);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    drain();
    const auto last_newline = observed.find('\n', first_bytes);
    const bool once_per_error = first_bytes == observed.find('\n') + 1 &&
        last_newline != std::string::npos && observed.size() == last_newline + 1;
    const bool retained = !acquired && mutex.test_owner_token() == 0 && mutex.test_gate_holder() == 0;
    sintra::detail::test_hooks::s_process_instance_fork_error = 0;
    worker.join();
    require(::dup2(original_stderr, STDERR_FILENO) >= 0, "cannot restore diagnostics");
    ::close(original_stderr);
    ::close(channel[0]);
    std::fprintf(stderr, "%s", observed.c_str());
    require(first_visible && change_visible && once_per_error && retained && acquired && mutex.unlock_owned(token),
        "mandatory acquisition silently retried a genuine error or lost retry custody");
    return 0;
}

int run_backoff_case(const case_t& test)
{
    const bool interrupted = std::string_view(test.name) == "native_backoff_interrupted";
    sintra::detail::test_hooks::s_native_backoff_attempts = 0;
    sintra::detail::test_hooks::s_native_backoff_fallbacks = 0;
    sintra::detail::test_hooks::s_native_backoff_error = interrupted ? EINTR : EINVAL;
    const auto before = std::chrono::steady_clock::now();
    sintra::detail::native_error_backoff();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    require(elapsed >= std::chrono::microseconds(500), "native cleanup error returned without a parking interval");
    require(interrupted ? (sintra::detail::test_hooks::s_native_backoff_attempts == 2 &&
        sintra::detail::test_hooks::s_native_backoff_fallbacks == 0) :
        (sintra::detail::test_hooks::s_native_backoff_attempts == 1 &&
        sintra::detail::test_hooks::s_native_backoff_fallbacks == 1),
        "native backoff did not retry interruption or park after a genuine API error");
    return 0;
}
#endif

int run_case(const std::string&, const case_t& test, const char*)
{
    const std::string_view action = test.action;
    if (action == "recursion") { return run_recursion_case(); }
    if (action == "gate_throw") { return run_gate_exception_case(test); }
    if (action == "timed_throw") { return run_timed_exception_case(test); }
#if !defined(_WIN32)
    if (action == "acquisition_error") { return run_mandatory_error_case(); }
    if (action == "backoff_error") { return run_backoff_case(test); }
#endif
    throw std::runtime_error("unknown mutex cleanup case");
}

} // namespace mutex_cleanup

int main(int argc, char* argv[])
{
    if (const auto result = sintra::test::isolated::run_cases(
        argc, argv, mutex_cleanup::k_cases, mutex_cleanup::run_case))
    {
        return *result;
    }
    return argc == 1 ? 0 : 1;
}
