#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#elif defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/user.h>
#endif

namespace {

namespace fs = std::filesystem;
using Reader = sintra::Ring_R<uint32_t>;
using Writer = sintra::Ring_W<uint32_t>;
using sintra::test::Exact_child;
using sintra::test::Exact_child_state;
constexpr const char* k_ring_name = "crash_wakeup";
constexpr uint32_t k_payload = 0x51a7u;
constexpr auto k_deadline = std::chrono::seconds(8);

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
bool await(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + k_deadline;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void signal_file(const fs::path& path)
{
    std::ofstream stream(path);
    stream << "ready\n";
    stream.close();
    require(bool(stream), "cannot publish fixture phase: " + path.string());
}

fs::path s_directory;
std::string s_poster_stage;
std::string s_reader_stage;
std::atomic<bool> s_reader_arrived{false};
std::atomic<bool> s_reader_released{false};
std::atomic<uint64_t> s_wait_thread{0};
std::atomic<uintptr_t> s_wait_address{0};
std::atomic<int> s_reader_index{-1};
thread_local bool tl_reader = false;

void wakeup_hook(const char* stage, const void* object)
{
    if (!s_poster_stage.empty() && s_poster_stage == stage) {
        signal_file(s_directory / "poster_seam");
        // The supervisor kills this exact process here, without a destructor.
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (!tl_reader) {
        return;
    }
    if (std::string_view(stage) == "semaphore_before_wait") {
#if defined(__linux__)
        s_wait_thread.store(uint64_t(::syscall(SYS_gettid)));
#elif defined(__APPLE__)
        s_wait_thread.store(uint64_t(pthread_mach_thread_np(pthread_self())));
#elif defined(__FreeBSD__)
        long thread_id = 0;
        require(thr_self(&thread_id) == 0, "thr_self failed");
        s_wait_thread.store(uint64_t(thread_id));
#endif
        s_wait_address.store(reinterpret_cast<uintptr_t>(object));
    }
    if (s_reader_stage == stage && !s_reader_arrived.exchange(true)) {
        while (!s_reader_released.load()) {
            std::this_thread::yield();
        }
    }
}

void reader_prepared(int index)
{
    s_reader_index = index;
    wakeup_hook("ring_wait_prepared", nullptr);
}

// A pre-syscall hook alone does not prove kernel parking. For the POSIX
// counter-before-wake case, inspect the reader's native state while no token,
// signal or competing poster can release it.
bool native_reader_is_parked()
{
    const auto thread_id = s_wait_thread.load();
    if (thread_id == 0 || s_wait_address.load() == 0) {
        return false;
    }
#if defined(__linux__)
    std::ifstream stream("/proc/self/task/" + std::to_string(thread_id) + "/syscall");
    long number = -1;
    uintptr_t address = 0;
    stream >> number >> std::hex >> address;
    return bool(stream) && number == SYS_futex && address == s_wait_address.load();
#elif defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    return thread_info(mach_port_t(thread_id), THREAD_BASIC_INFO,
               reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS &&
        info.run_state == TH_STATE_WAITING;
#elif defined(__FreeBSD__)
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD, int(getpid())};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) {
        return false;
    }
    std::vector<kinfo_proc> entries(size / sizeof(kinfo_proc) + 4);
    size = entries.size() * sizeof(kinfo_proc);
    if (sysctl(mib, 4, entries.data(), &size, nullptr, 0) != 0) {
        return false;
    }
    for (size_t i = 0; i < size / sizeof(kinfo_proc); ++i) {
        if (uint64_t(entries[i].ki_tid) == thread_id && entries[i].ki_stat == SSLEEP &&
            std::string_view(entries[i].ki_wmesg) == "uwait")
        {
            return true;
        }
    }
    return false;
#else
    return false;
#endif
}

class Probe_reader : public Reader
{
public:
    using Reader::Reader;

    // The raw-ring owner explicitly retries this existing flush. This fixture
    // does not claim native-death integration exists in the message transport.
    void replay()
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        require(m_control->flush_wakeups().count == 0, "notification replay returned a backend error");
    }

    bool token_empty()
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        const int index = s_reader_index.load();
        if (index < 0) {
            return m_control->sleeping_stack.empty();
        }
        return m_control->dirty_semaphores[index].wait_for(std::chrono::nanoseconds(0)) ==
            sintra::sintra_ring_semaphore::wait_result::timeout;
    }

    int pending_count()
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        return m_control->sleeping_stack.size();
    }

    // Called only by a reset hook while its posting lock is already held.
    int pending_count_during_reset() const
    {
        return m_control->sleeping_stack.size();
    }

    bool token_present()
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        const int index = s_reader_index.load();
        require(index >= 0, "token probe has no reader slot");
        auto& semaphore = m_control->dirty_semaphores[index];
        if (semaphore.wait_for(std::chrono::nanoseconds(0)) ==
            sintra::sintra_ring_semaphore::wait_result::timeout)
        {
            return false;
        }
        // Restore the observed token so the next registration must drain it.
        require(!semaphore.post(), "cannot restore the token after probing it");
        return true;
    }
};

class Probe_writer : public Writer
{
public:
    using Writer::Writer;

    int free_slots()
    {
        sintra::spinlock::locker lock(m_control->rs_stack_spinlock);
        return m_control->free_rs_stack.size();
    }
};

struct wakeup_case_t
{
    const char* name;
    const char* poster_stage;
    const char* action;
    const char* reader_stage;
    const char* retry;
};

constexpr wakeup_case_t k_cases[] = {
    {"post_failure", "", "checked", "", ""},
    {"post_failure_log_callback", "", "checked", "", ""},
    {"stop_post_failure", "", "checked", "", ""},
    {"close_post_failure", "", "checked", "", ""},
    {"reset_failure", "", "checked", "", ""},
    {"same_reader_reset_failure", "", "checked", "", ""},
    {"rpc_request_post_failure", "", "rpc", "", ""},
    {"rpc_reply_post_failure", "", "rpc", "", ""},
    {"interrupted_post_stop", "ring_post_before_backend", "data", "semaphore_before_wait", "stop"},
    {"registration_replay", "ring_flush_before_post", "data", "semaphore_before_wait", "flush"},
    {"successor_replay", "ring_commit_published", "data", "semaphore_before_wait", "successor"},
    {"duplicate_post", "ring_flush_after_post", "data", "semaphore_before_wait", "flush"},
    {"close_late_registration", "ring_close_published", "close", "ring_wait_before_register", "none"},
    {"unblock_late_registration", "ring_unblock_published", "unblock", "ring_wait_before_register", "none"},
    {"close_replay", "ring_close_published", "close", "semaphore_before_wait", "flush"},
    {"unblock_replay", "ring_unblock_published", "unblock", "semaphore_before_wait", "flush"},
#ifdef _WIN32
    {"handle_allocation_failure", "", "checked", "", ""},
    {"admission_failure", "", "checked", "", ""},
    {"posted_object_lifetime", "", "data", "ring_wait_prepared", "none"},
#else
    {"post_token_failure", "", "checked", "", ""},
    {"published_token_kernel_wake", "semaphore_count_published", "data", "", "local"},
#endif
};

[[noreturn]] void fixture_failure(const std::string& message)
{
    std::fprintf(stderr, "checked wakeup fixture: %s\n", message.c_str());
    std::fflush(stderr);
    std::_Exit(2);
}

std::atomic<unsigned> s_post_failures{0};
std::atomic<unsigned> s_failure_waiters{0};
bool s_require_native_wait = false;

void failure_hook(const char* stage, const void* object)
{
    if (std::string_view(stage) == "semaphore_post_failed" ||
        std::string_view(stage) == "semaphore_handle_allocation_failed")
    {
        ++s_post_failures;
        if (std::string_view(stage) == "semaphore_handle_allocation_failed") {
            std::printf("Windows handle allocation failure inside helper\n");
        }
        std::printf("backend post failure observed\n");
        std::fflush(stdout);
    }
    if (s_require_native_wait) {
        wakeup_hook(stage, object);
    }
    else
    if (tl_reader && std::string_view(stage) == "semaphore_before_wait") {
        ++s_failure_waiters;
        while (!s_reader_released.load()) {
            std::this_thread::yield();
        }
    }
}

void stop_from_log(sintra::log_level, const char*, void* reader)
{
    std::fprintf(stderr, "application log callback requests reader stop\n");
    std::fflush(stderr);
    static_cast<Probe_reader*>(reader)->request_stop();
}

Probe_reader* s_reset_reader = nullptr;
std::atomic<unsigned> s_reset_attempts{0};
std::atomic<unsigned> s_reset_failures{0};
std::atomic<unsigned> s_reset_waits{0};

void same_reader_reset_hook(const char* stage, const void* object)
{
    if (!tl_reader) {
        return;
    }
    const std::string_view event = stage;
    if (event == "ring_reset_before_backend") {
        const auto attempt = ++s_reset_attempts;
        if (s_reset_reader->pending_count_during_reset() != 0) {
            fixture_failure("reset ran after the next sleeping registration was published");
        }
        if (attempt == 1) {
            auto* semaphore = const_cast<sintra::sintra_ring_semaphore*>(
                static_cast<const sintra::sintra_ring_semaphore*>(object));
            if (semaphore->post()) {
                fixture_failure("cannot leave a token at the failed cleanup seam");
            }
            sintra::detail::test_hooks::s_ipc_reset_error = EIO;
        }
    }
    if (event == "semaphore_reset_failed") {
        ++s_reset_failures;
    }
    if (event == "semaphore_before_wait") {
        const auto wait = ++s_reset_waits;
        if (wait == 1) {
            while (!s_reader_released.load()) {
                std::this_thread::yield();
            }
        }
        if (wait == 2 && s_reset_attempts != 2) {
            fixture_failure("same reader registered without retrying its failed cleanup");
        }
    }
}

void test_same_reader_reset_failure(const char* directory, size_t capacity)
{
    namespace hooks = sintra::detail::test_hooks;
    Probe_reader reader(directory, k_ring_name, capacity);
    reader.start_reading();
    s_reset_reader = &reader;
    hooks::s_ipc_wakeup_operation = same_reader_reset_hook;
    std::atomic<bool> completed{false};
    std::atomic<bool> valid{false};
    std::thread first_wait([&] {
        tl_reader = true;
        try {
            const auto range = reader.wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
            valid = range.begin == range.end && !reader.is_stopping();
            completed = true;
        }
        catch (const std::exception& error) {
            fixture_failure(error.what());
        }
    });
    if (!await([] { return s_reset_waits == 1; })) {
        fixture_failure("same-reader fixture did not reach its first wait seam");
    }
    reader.unblock_local();
    s_reader_released = true;
    first_wait.join();
    require(completed && valid && s_reset_attempts == 1 && s_reset_failures == 1,
        "same-reader cleanup did not return after the injected reset failure");
    require(reader.pending_count() == 0 && reader.token_present(),
        "failed same-reader cleanup did not leave the token and reset obligation");

    completed = false;
    valid = false;
    std::thread second_wait([&] {
        tl_reader = true;
        try {
            const auto range = reader.wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
            valid = range.begin == range.end && reader.is_stopping();
            completed = true;
        }
        catch (const std::exception& error) {
            fixture_failure(error.what());
        }
    });
    if (!await([] { return s_reset_waits == 2; })) {
        fixture_failure("same reader did not drain the stale token and reach a second wait");
    }
    reader.request_stop();
    second_wait.join();
    require(completed && valid && s_reset_attempts == 3 && s_reset_failures == 1,
        "same reader did not retry reset before registration and finish stop cleanup");
    require(reader.pending_count() == 0 && reader.token_empty(),
        "successful same-reader retry left a stale registration or token");
    hooks::s_ipc_wakeup_operation = nullptr;
    s_reset_reader = nullptr;
    reader.done_reading();
}

int run_checked_case(const wakeup_case_t& test, const char* directory)
{
    namespace hooks = sintra::detail::test_hooks;
    const std::string_view name = test.name;
    hooks::s_ring_wait_watchdog_enabled = false;
    hooks::s_ipc_wakeup_operation = failure_hook;
    hooks::s_ring_wait_prepared = reader_prepared;
    const auto capacity = sintra::test::pick_ring_elements<uint32_t>();
    auto writer = std::make_unique<Probe_writer>(directory, k_ring_name, capacity);

    if (name == "same_reader_reset_failure") {
        test_same_reader_reset_failure(directory, capacity);
        return 0;
    }

    if (name == "reset_failure" || name == "admission_failure") {
        const int free_before = writer->free_slots();
        if (name == "reset_failure") {
            auto reader = std::make_unique<Probe_reader>(directory, k_ring_name, capacity);
            hooks::s_ipc_reset_error = EIO;
            reader.reset();
            require(writer->free_slots() == free_before, "failed reset leaked the released slot");
        }
        bool rejected = false;
        if (name == "reset_failure") {
            hooks::s_ipc_reset_error = EIO;
        }
        else {
            hooks::s_ipc_prepare_error = EACCES;
        }
        try {
            Probe_reader reader(directory, k_ring_name, capacity);
        }
        catch (const std::system_error& error) {
            rejected = error.code().value() == (name == "reset_failure" ? EIO : EACCES);
        }
        require(rejected && writer->free_slots() == free_before,
            "failed reader admission published or lost a lifetime slot");
        {
            Probe_reader reader(directory, k_ring_name, capacity);
            require(writer->free_slots() == free_before - 1,
                "reader could not reuse the slot after successful checked reset");
        }
        return 0;
    }

    const bool stop = name == "stop_post_failure";
    const bool close = name == "close_post_failure";
    const bool after_token = name == "post_token_failure";
    const bool log_callback = name == "post_failure_log_callback";
    const bool allocation_failure = name == "handle_allocation_failure";
    const unsigned count = stop || close || after_token ? 1 : 2;
    s_require_native_wait = after_token;
    std::vector<std::shared_ptr<Probe_reader>> readers;
    std::vector<std::thread> waiters;
    std::atomic<unsigned> completed{0};
    std::atomic<unsigned> valid{0};
    std::atomic<unsigned> empty_ranges{0};
    std::atomic<unsigned> stopped_on_return{0};
    for (unsigned i = 0; i != count; ++i) {
        auto reader = std::make_shared<Probe_reader>(directory, k_ring_name, capacity);
        reader->start_reading();
        readers.push_back(reader);
        if (log_callback && i == 0) {
            sintra::set_log_callback(stop_from_log, reader.get());
        }
        waiters.emplace_back([&, reader, i] {
            tl_reader = true;
            try {
                const auto range = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
                if (stop || close) {
                    bool empty = range.begin == range.end;
                    bool stopping = reader->is_stopping();
                    if (close && !stopping) {
                        // A wake can return empty before the next call observes close.
                        const auto end = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
                        empty = empty && end.begin == end.end;
                        stopping = reader->is_stopping();
                    }
                    empty_ranges += empty;
                    stopped_on_return += stopping;
                    valid += empty && stopping;
                }
                else {
                    const bool delivered = range.begin && range.end - range.begin == 1 &&
                        *range.begin == k_payload;
                    const bool stopped = log_callback && i == 0 && range.begin == range.end &&
                        reader->is_stopping();
                    valid += delivered || stopped;
                }
                ++completed;
            }
            catch (const std::exception& error) {
                fixture_failure(error.what());
            }
        });
        if (log_callback && !await([&] { return s_failure_waiters == i + 1; })) {
            fixture_failure("callback fixture did not order its reader registrations");
        }
    }
    require(await([&] { return after_token ? native_reader_is_parked() : s_failure_waiters == count; }),
        "failure fixture readers did not reach their wait seam");
    if (after_token) {
        std::printf("post_token_failure: native wait confirmed\n");
        std::fflush(stdout);
        hooks::s_ipc_binary_wake_error = EIO;
    }
    else
    if (allocation_failure) {
        hooks::s_ipc_handle_allocation_failure = true;
    }
    else {
        hooks::s_ipc_binary_post_error = EIO;
    }
    try {
        if (stop) {
            readers.front()->request_stop();
        }
        else
        if (close) {
            writer.reset();
        }
        else {
            require(writer->write_commit(k_payload) == 1, "notification failure changed the committed head");
        }
    }
    catch (const std::exception& error) {
        fixture_failure(std::string("published operation threw a notification error: ") + error.what());
    }
    require(s_post_failures == 1, "intended backend post failure was not observed");
    require(readers.front()->pending_count() != 0, "failed notification lost its registration");
    s_reader_released = true;
    if (count == 2) {
        require(await([&] { return completed == 1; }), "failed post prevented an unaffected reader wake");
    }
    if (stop) {
        readers.front()->request_stop();
    }
    else {
        readers.front()->replay();
    }
    for (auto& waiter : waiters) {
        waiter.join();
    }
    if (log_callback) {
        sintra::set_log_callback(nullptr);
    }
    require(readers.front()->pending_count() == 0,
        "notification-only replay left a registration");
    require(valid == count,
        "notification-only replay returned the wrong outcome: valid=" + std::to_string(valid.load()) +
        " empty=" + std::to_string(empty_ranges.load()) +
        " stopping=" + std::to_string(stopped_on_return.load()) +
        " expected=" + std::to_string(count));
    for (auto& reader : readers) {
        reader->done_reading();
    }
    if (close) {
        writer = std::make_unique<Probe_writer>(directory, k_ring_name, capacity);
    }
    else
    if (!stop) {
        std::thread next_writer([&] {
            require(writer->write_commit(k_payload + 1) == 2,
                "subsequent writer did not preserve the committed head");
        });
        next_writer.join();
    }
    if (allocation_failure) {
        const int free_before = writer->free_slots();
        hooks::s_ipc_handle_allocation_failure = true;
        bool rejected = false;
        try {
            Probe_reader reader(directory, k_ring_name, capacity);
        }
        catch (const std::system_error& error) {
            rejected = error.code().value() == ENOMEM;
        }
        require(rejected && writer->free_slots() == free_before,
            "handle allocation failure bypassed reader admission rollback");
        Probe_reader reader(directory, k_ring_name, capacity);
        require(writer->free_slots() == free_before - 1,
            "reader slot was not reusable after handle allocation failure");
    }
    return 0;
}

std::atomic<unsigned> s_rpc_calls{0};
std::atomic<unsigned> s_rpc_fallbacks{0};
std::atomic<bool> s_rpc_request_reparked{false};
std::atomic<bool> s_rpc_gate_armed{false};
bool s_rpc_reply_case = false;

struct Wakeup_service : sintra::Derived_transceiver<Wakeup_service>
{
    int ping(int value, bool fail_reply)
    {
        ++s_rpc_calls;
        if (fail_reply) {
            sintra::detail::test_hooks::s_ipc_binary_post_error = EIO;
        }
        return value + 1;
    }
    SINTRA_RPC_STRICT(ping)
};

void rpc_hook(const char* stage, const void*)
{
    const std::string_view event = stage;
    if (event == "semaphore_post_failed") {
        ++s_post_failures;
        std::printf("RPC backend post failure observed\n");
        std::fflush(stdout);
    }
}

void rpc_reader_prepared(int)
{
    if (s_rpc_reply_case && sintra::tl_is_req_thread && s_rpc_calls == 2) {
        s_rpc_request_reparked = true;
    }
    // This fixture creates one process and its own request/reply reader pair.
    // Reader and writer control mappings have different virtual addresses.
    if (s_rpc_gate_armed && sintra::tl_is_req_thread != s_rpc_reply_case &&
        !s_reader_arrived.exchange(true))
    {
        while (!s_reader_released.load()) {
            std::this_thread::yield();
        }
    }
}

int run_rpc_case(const std::string& binary, const wakeup_case_t& test, const char* directory)
{
    namespace hooks = sintra::detail::test_hooks;
    fs::current_path(directory);
    const char* runtime_args[] = {binary.c_str(), nullptr};
    hooks::s_ring_wait_watchdog_enabled = false;
    std::printf("RPC fixture: initializing\n");
    std::fflush(stdout);
    sintra::init(1, runtime_args);
    std::printf("RPC fixture: initialized\n");
    std::fflush(stdout);
    {
        Wakeup_service service;
        std::printf("RPC fixture: service constructed\n");
        std::fflush(stdout);
        require(Wakeup_service::rpc_ping(service.instance_id(), 10, false) == 11,
            "RPC prewarm failed");
        std::printf("RPC fixture: prewarm complete\n");
        std::fflush(stdout);
        s_rpc_reply_case = std::string_view(test.name) == "rpc_reply_post_failure";
        auto& ring = s_rpc_reply_case ? sintra::s_mproc->m_out_rep_c : sintra::s_mproc->m_out_req_c;
        hooks::s_ipc_wakeup_operation = rpc_hook;
        hooks::s_ring_wait_prepared = rpc_reader_prepared;
        hooks::s_rpc_response_stage = [](const char* stage) {
            if (std::string_view(stage) == hooks::k_stage_rpc_response_before_fallback) {
                ++s_rpc_fallbacks;
            }
        };
        s_rpc_gate_armed = true;
        ring->unblock_global();
        require(await([] { return s_reader_arrived.load(); }), "RPC reader registration gate was not reached");
        std::printf("RPC fixture: reader registered\n");
        std::fflush(stdout);
        if (!s_rpc_reply_case) {
            hooks::s_ipc_binary_post_error = EIO;
        }
        sintra::Rpc_handle<int> handle;
        try {
            handle = Wakeup_service::rpc_async_ping(service.instance_id(), 40, s_rpc_reply_case);
        }
        catch (const std::exception& error) {
            fixture_failure(std::string("committed async request lost its handle: ") + error.what());
        }
        if (s_rpc_reply_case) {
            require(await([] { return s_rpc_request_reparked.load(); }), "reply writer did not finish dispatch");
        }
        require(s_post_failures == 1, "RPC did not exercise the intended post failure");
        if (s_rpc_fallbacks != 0) {
            fixture_failure("committed reply entered serialization fallback");
        }
        {
            std::lock_guard lock(sintra::s_outstanding_rpcs_mutex());
            require(sintra::s_outstanding_rpcs().size() == 1,
                "committed request lost its outstanding async state");
            auto* control = *sintra::s_outstanding_rpcs().begin();
            std::lock_guard state_lock(control->keep_waiting_mutex);
            require(control->keep_waiting && !control->abandoned &&
                    control->remote_instance == service.instance_id(),
                "committed request was cancelled, abandoned or associated with another target");
        }
        ring->unblock_global();
        s_reader_released = true;
        require(handle.get() == 41 && s_rpc_calls == 2 && s_rpc_fallbacks == 0,
            "RPC notification retry changed delivery or response count");
        hooks::s_ring_wait_prepared = nullptr;
        hooks::s_ipc_wakeup_operation = nullptr;
        hooks::s_rpc_response_stage = nullptr;
    }
    require(sintra::detail::finalize_impl(), "RPC fixture runtime teardown failed");
    return 0;
}

const wakeup_case_t& find_case(std::string_view name)
{
    for (const auto& test : k_cases) {
        if (name == test.name) {
            return test;
        }
    }
    throw std::runtime_error("unknown wakeup case: " + std::string(name));
}

int run_poster(const wakeup_case_t& test, const char* directory)
{
    s_directory = directory;
    {
        Writer writer(directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>());
        signal_file(s_directory / "poster_ready");
        require(await([] { return fs::exists(s_directory / "start_post"); }),
            "poster did not receive its start phase");
        s_poster_stage = test.poster_stage;
        sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
        if (std::string_view(test.action) == "data") {
            writer.write_commit(k_payload);
        }
        else
        if (std::string_view(test.action) == "unblock") {
            writer.unblock_global();
        }
        if (s_poster_stage.empty()) {
            // Release every process-local handle without a close notification.
            std::_Exit(0);
        }
    }
    throw std::runtime_error("poster failed to reach the requested interruption seam");
}

int run_scenario(const std::string& binary, const wakeup_case_t& test, const char* directory)
{
    if (std::string_view(test.action) == "checked") {
        return run_checked_case(test, directory);
    }
    if (std::string_view(test.action) == "rpc") {
        return run_rpc_case(binary, test, directory);
    }
    s_directory = directory;
    s_reader_stage = test.reader_stage;
    sintra::detail::test_hooks::s_ring_wait_watchdog_enabled = false;
    sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
    sintra::detail::test_hooks::s_ring_wait_prepared = reader_prepared;
    Exact_child poster(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), "--poster", test.name, directory, nullptr};
    require(poster.spawn(binary.c_str(), args), "cannot spawn poster: " + poster.error());
    require(await([] { return fs::exists(s_directory / "poster_ready"); }),
        "poster did not acquire writer ownership");

    auto reader = std::make_shared<Probe_reader>(
        directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>());
    reader->start_reading();
    std::atomic<bool> completed{false};
    std::atomic<bool> valid{false};
    std::thread waiter([
            reader,
            &completed,
            &valid,
            &test
        ]()
        {
            tl_reader = true;
            try {
                const auto range = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
                const bool has_data = range.begin && range.end - range.begin == 1 &&
                    *range.begin == k_payload;
                valid = std::string_view(test.retry) == "stop"
                    ? reader->is_stopping()
                    : std::string_view(test.action) == "data" ? has_data : range.begin == range.end;
                if (std::string_view(test.action) == "close" && !reader->is_stopping()) {
                    const auto end = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
                    valid = valid && end.begin == end.end && reader->is_stopping();
                }
            }
            catch (const std::exception& error) {
                std::fprintf(stderr, "waiter: %s\n", error.what());
            }
            completed = true;
        });

    // On a fixture failure the outer exact-child supervisor owns termination;
    // avoid destroying a joinable thread while reporting that failure.
    const auto fail = [&](const char* message) {
        std::fprintf(stderr, "%s: %s\n", test.name, message);
        std::string diagnostic;
        (void)poster.terminate_and_settle(diagnostic);
        std::_Exit(2);
    };
    if (s_reader_stage.empty()) {
        if (!await(native_reader_is_parked)) {
            fail("native reader parking could not be established");
        }
        std::printf("%s: native wait confirmed\n", test.name);
        std::fflush(stdout);
    }
    else
    if (!await([] { return s_reader_arrived.load(); })) {
        fail("reader did not reach its controlled wait seam");
    }
    signal_file(s_directory / "start_post");
    if (std::string_view(test.poster_stage).empty()) {
        if (!await([&] { return poster.poll() == Exact_child_state::exited; })) {
            fail("poster did not exit after publishing");
        }
        if (!poster.exited_with_code(0)) {
            fail("poster failed before exiting normally");
        }
    }
    else {
        if (!await([] { return fs::exists(s_directory / "poster_seam"); })) {
            fail("poster did not reach its controlled crash seam");
        }
    }
    std::string diagnostic;
    if (!poster.terminate_and_settle(diagnostic)) {
        fail(diagnostic.c_str());
    }

    std::unique_ptr<Writer> successor;
    const std::string_view retry = test.retry;
    if (retry == "stop") {
        reader->request_stop();
    }
    else
    if (retry == "flush") {
        reader->replay();
    }
    else
    if (retry == "local") {
        reader->unblock_local();
    }
    else
    if (retry == "successor") {
        successor = std::make_unique<Writer>(
            directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>());
    }
    s_reader_released = true;
    // A baseline missed wake blocks here. The parent reports that failure;
    // neither this process nor its reader uses a timeout to make progress.
    waiter.join();
    require(completed && valid, "reader returned the wrong wake outcome");
    require(reader->token_empty(), "cleanup left a stale token for the next registration");
    reader->done_reading();
    return 0;
}

bool supervise_case(const std::string& binary, const wakeup_case_t& test)
{
    sintra::test::Temp_ring_dir directory(test.name);
    const std::string path = directory.str();
    Exact_child scenario(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), "--scenario", test.name, path.c_str(), nullptr};
    require(scenario.spawn(binary.c_str(), args), "cannot spawn scenario: " + scenario.error());
    const bool exited = await([&] { return scenario.poll() != Exact_child_state::running; });
    const bool passed = exited && scenario.exited_with_code(0);
    if (!passed) {
        std::fprintf(stderr, "%s: %s\n", test.name,
            exited ? scenario.describe_status().c_str() : "supervisor deadline: reader made no progress");
    }
    std::string diagnostic;
    require(scenario.terminate_and_settle(diagnostic), diagnostic);
    std::printf("%s: %s\n", test.name, passed ? "PASS" : "FAIL");
    std::fflush(stdout);
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto binary = sintra::test::get_binary_path(argc, argv);
        if (argc == 4 && std::string_view(argv[1]) == "--poster") {
            return run_poster(find_case(argv[2]), argv[3]);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--scenario") {
            return run_scenario(binary, find_case(argv[2]), argv[3]);
        }
        if (argc == 3 && std::string_view(argv[1]) == "--case") {
            return supervise_case(binary, find_case(argv[2])) ? 0 : 1;
        }
        require(argc == 1, "expected --case NAME or no arguments");
        bool passed = true;
        for (const auto& test : k_cases) {
            passed = supervise_case(binary, test) && passed;
        }
        return passed ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "ring_crash_safe_wakeup_test: %s\n", error.what());
        return 1;
    }
}
