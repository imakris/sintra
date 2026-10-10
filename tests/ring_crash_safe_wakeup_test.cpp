#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Arm immediately before the actual allocating expression under test. The
// constructor must unwind the resulting allocator failure, rather than swallow
// a throwing observation and continue successfully.
thread_local bool s_fail_next_allocation = false;
thread_local unsigned s_successful_allocations_before_failure = 0;

void* operator new(std::size_t bytes)
{
    if (s_fail_next_allocation) {
        if (s_successful_allocations_before_failure != 0) {
            --s_successful_allocations_before_failure;
        }
        else {
            s_fail_next_allocation = false;
            throw std::bad_alloc();
        }
    }
    if (void* memory = std::malloc(bytes ? bytes : 1)) { return memory; }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#if defined(_WIN32)
#include <winternl.h>
#elif defined(__APPLE__)
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
using Native_notification = sintra::detail::Ring_native_notification<uint32_t>;
using sintra::test::Exact_child;
using sintra::test::Exact_child_state;
constexpr const char* k_ring_name = "crash_wakeup";
constexpr uint32_t k_payload = 0x51a7u;
constexpr auto k_deadline = std::chrono::seconds(8);

class Probe_notification : public Native_notification
{
public:
    using Native_notification::Native_notification;
    uint64_t gate_holder() const { return this->m_control->ownership_mutex.test_gate_holder(); }
    uint64_t posting_owner() const { return this->m_control->m_spinlock.test_owner(); }
    sintra::spinlock& reader_lock() { return this->m_control->rs_stack_spinlock; }
    sintra::detail::interprocess_mutex& ownership_mutex() { return this->m_control->ownership_mutex; }
};

class Armed_child_witness : public sintra::detail::Native_exit_witness
{
public:
    Armed_child_witness(uint64_t instance, std::shared_ptr<Exact_child> child)

    :
        Native_exit_witness(instance),
        m_child(std::move(child))
    {}

    bool has_exited() const noexcept override
    {
        return m_child->observe_exit_retained() == Exact_child_state::exited;
    }

private:
    const std::shared_ptr<Exact_child> m_child;
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "fixture check failed: %s\n", message.c_str());
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
bool s_recoverer = false;
std::string s_recoverer_label = "recoverer";
const void* s_ownership_mutex = nullptr;
Probe_notification* s_exception_notification = nullptr;
const char* s_exception_stage = nullptr;
std::atomic<unsigned> s_native_log_callbacks{0};
std::atomic<uint64_t> s_diagnostic_cpu{0};
std::atomic<bool> s_held_logger{false};
std::atomic<bool> s_expect_held_logger{false};
std::atomic<bool> s_completion_arrived{false};
std::atomic<bool> s_completion_continue{false};
std::atomic<bool> s_new_enrollment_arrived{false};
thread_local bool tl_new_enrollment = false;

void exception_termination()
{
    if (auto error = std::current_exception()) {
        try {
            std::rethrow_exception(error);
        }
        catch (const std::exception& failure) {
            std::fprintf(stderr, "termination exception: %s; tid=%u\n", failure.what(), sintra::get_current_tid());
        }
        catch (...) {
            std::fprintf(stderr, "termination exception: non-standard\n");
        }
    }
    std::fprintf(stderr,
        "%s: noexcept termination observed before outer cleanup; gate=%llu self=%llu pending=%d posting_owner=%llu\n",
        s_exception_stage,
        static_cast<unsigned long long>(s_exception_notification->gate_holder()),
        static_cast<unsigned long long>(sintra::detail::current_process_instance()),
        int(s_exception_notification->pending()),
        static_cast<unsigned long long>(s_exception_notification->posting_owner()));
    std::fflush(stderr);
    std::_Exit(79);
}

void diagnostic_abort(int)
{
    const bool no_callback = s_native_log_callbacks == 0;
    sintra::detail::native_diagnostic("native live-stall diagnostic: application_callbacks=",
        s_native_log_callbacks.load(), "\n");
    std::_Exit(no_callback ? 0 : 78);
}

void wakeup_hook(const char* stage, const void* object)
{
    if (std::string_view(stage) == "ring_writer_before_ownership") {
        s_ownership_mutex = object;
        return;
    }
    if (!s_poster_stage.empty() && s_poster_stage == stage) {
        signal_file(s_directory / (s_recoverer ? s_recoverer_label + "_seam" : "poster_seam"));
        // The supervisor kills this exact process here, without a destructor.
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (!tl_reader) {
        return;
    }
    if (std::string_view(stage) == "semaphore_before_wait") {
#if defined(_WIN32)
        s_wait_thread.store(GetCurrentThreadId());
#elif defined(__linux__)
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
#if defined(_WIN32)
    // The SDK documents process records followed by their native thread records:
    // https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation
    using Query = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) {
        return false;
    }
    std::vector<unsigned char> buffer(256 * 1024);
    ULONG required = 0;
    auto status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &required);
    if (status < 0 && required > buffer.size()) {
        buffer.resize(required);
        status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &required);
    }
    if (status < 0) {
        return false;
    }
    size_t offset = 0;
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= buffer.size()) {
        const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
        if (reinterpret_cast<uintptr_t>(process->UniqueProcessId) == GetCurrentProcessId()) {
            const auto* threads = reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(process + 1);
            if (process->NumberOfThreads >
                (buffer.size() - offset - sizeof(*process)) / sizeof(*threads))
            {
                return false;
            }
            for (ULONG i = 0; i != process->NumberOfThreads; ++i) {
                if (reinterpret_cast<uintptr_t>(threads[i].ClientId.UniqueThread) == thread_id) {
                    // Native Waiting state (System.Diagnostics.ThreadState.Wait = 5).
                    return threads[i].ThreadState == 5;
                }
            }
            return false;
        }
        if (process->NextEntryOffset == 0 || process->NextEntryOffset > buffer.size() - offset) {
            return false;
        }
        offset += process->NextEntryOffset;
    }
    return false;
#elif defined(__linux__)
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

    bool copy_committed_first(uint32_t& owned)
    {
        Local_read_lock lock(m_reading_lock);
        return with_copying_mark([&] { owned = *this->get_base_address(); }) == Copy_admission::COPIED;
    }

    uint64_t guard_counts() const { return m_control->read_access.load(); }
    uint64_t posting_owner() const { return m_control->m_spinlock.test_owner(); }
    uint64_t reader_slot_owner() const { return m_control->rs_stack_spinlock.test_owner(); }
    uint64_t inspection_holder() const { return m_control->ownership_mutex.test_gate_holder(); }
    uint64_t writer_owner() const { return m_control->ownership_mutex.test_owner_token(); }
    int pending_count_unlocked() const { return m_control->sleeping_stack.size(); }
    sintra::spinlock& posting_lock() { return m_control->m_spinlock; }
    void seed_registration(int index)
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        m_control->sleeping_stack.push(index);
    }

    // The raw-ring owner explicitly retries this existing flush. This fixture
    // does not claim native-death integration exists in the message transport.
    void replay()
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        require(m_control->flush_wakeups().count == 0, "notification replay returned a backend error");
    }

    bool token_signaled_at(int index)
    {
        return m_control->dirty_semaphores[index].wait_for(std::chrono::nanoseconds(0)) ==
            sintra::sintra_ring_semaphore::wait_result::signaled;
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

    uint64_t guard_counts() const { return m_control->read_access.load(); }
    int free_slots_unlocked() const { return m_control->free_rs_stack.size(); }
    int pending_count_unlocked() const { return m_control->sleeping_stack.size(); }
    uint64_t posting_owner() const { return m_control->m_spinlock.test_owner(); }
    uint64_t reader_slot_owner() const { return m_control->rs_stack_spinlock.test_owner(); }
    uint64_t inspection_holder() const { return m_control->ownership_mutex.test_gate_holder(); }
    uint64_t replace_control_abi(uint64_t value) { return m_control->abi_fingerprint.exchange(value); }
    void post_token(int index) { require(!m_control->dirty_semaphores[index].post(), "cannot seed retirement token"); }
    bool consume_token(int index)
    {
        return m_control->dirty_semaphores[index].wait_for(std::chrono::nanoseconds(0)) ==
            sintra::sintra_ring_semaphore::wait_result::signaled;
    }

    int free_slots()
    {
        sintra::spinlock::locker lock(m_control->rs_stack_spinlock);
        return m_control->free_rs_stack.size();
    }
};

Probe_writer* s_reset_report_writer = nullptr;
std::atomic<unsigned> s_reset_reports{0};
std::atomic<bool> s_reset_report_with_gate{false};

void observe_reset_report(const char* stage)
{
    if (std::string_view(stage) == "ring_wakeup_error_report") {
        ++s_reset_reports;
        if (s_reset_report_writer->posting_owner() != 0 ||
            s_reset_report_writer->reader_slot_owner() != 0 ||
            s_reset_report_writer->inspection_holder() != 0)
        {
            s_reset_report_with_gate = true;
        }
    }
}

struct wakeup_case_t
{
    const char* name;
    const char* poster_stage;
    const char* action;
    const char* reader_stage;
    const char* retry;
};

constexpr wakeup_case_t k_cases[] = {
    {"native_exhausted_admission_proof_reset", "", "exhausted_admission", "", ""},
#if !defined(_WIN32)
    {"native_multithreaded_fork_exec", "", "fork_exec", "", ""},
#endif
    {"data_first_mapping_allocation_unwind", "", "construction", "", ""},
    {"control_mapping_allocation_unwind", "", "construction", "", ""},
    {"control_abi_diagnostic_allocation_unwind", "", "construction", "", ""},
    {"data_attach_marker_allocation_cleanup", "", "construction", "", ""},
    {"before_release_marker_allocation_cleanup", "", "construction", "", ""},
    {"after_release_marker_allocation_cleanup", "", "construction", "", ""},
    {"private_named_path_allocation_unwind", "", "construction", "", ""},
#if defined(__linux__) && defined(MAP_SYNC) && defined(MAP_SHARED_VALIDATE) && defined(MAP_FIXED_NOREPLACE)
    {"data_native_mapping_failure_reuse", "", "construction", "", ""},
#endif
#if !defined(_WIN32)
    {"private_opened_fd_allocation_unwind", "", "construction", "", ""},
    {"data_second_mapping_allocation_unwind", "", "construction", "", ""},
    {"mandatory_acquisition_real_error", "", "acquisition_error", "", ""},
    {"native_backoff_interrupted", "", "backoff_error", "", ""},
    {"native_backoff_genuine_error", "", "backoff_error", "", ""},
#endif
    {"writer_constructor_before_cas_cleanup", "mutex_before_owner_cas", "custody", "", ""},
    {"writer_constructor_after_cas_cleanup", "mutex_after_owner_cas", "custody", "", ""},
    {"lifecycle_before_cas_cleanup", "mutex_before_owner_cas", "custody", "", ""},
    {"lifecycle_after_cas_cleanup", "mutex_after_owner_cas", "custody", "", ""},
    {"reader_admission_reset_failure", "", "custody", "", ""},
    {"writer_close_multiple_slot_errors", "", "close_errors", "", ""},
    {"writer_close_backend_retry", "", "retirement", "", ""},
    {"reader_stop_backend_retry", "", "retirement", "", ""},
    {"mutex_timed_before_cas", "mutex_before_owner_cas", "timed_throw", "", ""},
    {"mutex_timed_after_cas", "mutex_after_owner_cas", "timed_throw", "", ""},
    {"reader_release_throw_cleanup", "", "retirement", "", ""},
    {"reader_retirement_reset_retry", "", "retirement", "", ""},
    {"reader_retirement_persistent_reset", "", "retirement", "", ""},
    {"writer_close_throw_cleanup", "", "retirement", "", ""},
    {"writer_close_flush_throw_cleanup", "", "retirement", "", ""},
    {"writer_close_post_throw_cleanup", "", "retirement", "", ""},
#if !defined(_WIN32)
    {"writer_close_count_throw_cleanup", "", "retirement", "", ""},
#endif
    {"native_signal_handler_callback", "ring_commit_published", "data", "", "native"},
#if defined(_WIN32)
    {"native_exception_handler_callback", "ring_commit_published", "data", "", "native"},
#endif
    {"mutex_acquire_throw_cleanup", "mutex_before_owner_cas", "gate_throw", "", ""},
    {"mutex_recovery_throw_cleanup", "mutex_recovery_gate_acquired", "gate_throw", "", ""},
    {"native_flush_throw_cleanup", "ring_commit_published", "data", "", "native"},
    {"native_backend_throw_cleanup", "ring_commit_published", "data", "", "native"},
    {"native_release_throw_cleanup", "ring_commit_published", "data", "", "native"},
    {"native_live_stall_callback", "ring_commit_published", "data", "", "native"},
    {"native_owner_before_cas", "mutex_before_owner_cas", "publication", "", "native"},
    {"native_owner_after_cas", "mutex_after_owner_cas", "publication", "", "native"},
    {"native_owner_repeated_recoverers", "mutex_inspection_gate_acquired", "publication_recoverer", "", "native"},
    {"mutex_mixed_recursion", "", "recursion", "", ""},
    {"native_parked_replay", "ring_commit_published", "data", "", "native"},
    {"native_empty_late_registration", "", "exit", "ring_wait_before_register", "native"},
    {"native_request_death", "request_published", "request", "", "native"},
    {"native_successor_rejection", "ring_commit_published", "data", "semaphore_before_wait", "native"},
    {"native_capability_setup", "", "capability", "", "native"},
    {"native_recoverer_edge", "ring_native_exit_published", "recoverer", "", "native"},
    {"native_recoverer_before_post", "ring_flush_before_post", "recoverer", "", "native"},
    {"native_recoverer_after_post", "ring_flush_after_post", "recoverer", "", "native"},
    {"native_recoverer_completion", "ring_native_replay_before_complete", "recoverer", "", "native"},
    {"native_commit_replay", "ring_commit_published", "data", "semaphore_before_wait", "native"},
    {"native_post_replay", "ring_post_before_backend", "data", "semaphore_before_wait", "native"},
    {"native_duplicate_replay", "ring_flush_after_post", "data", "semaphore_before_wait", "native"},
    {"native_late_registration", "ring_commit_published", "data", "ring_wait_before_register", "native"},
    {"native_late_enrollment", "ring_commit_published", "data", "semaphore_before_wait", "native"},
    {"native_completion_new_enrollment", "ring_commit_published", "data", "semaphore_before_wait", "native"},
    {"native_held_logger_handler", "ring_commit_published", "data", "semaphore_before_wait", "native"},
    {"native_transient_failure", "ring_commit_published", "data", "semaphore_before_wait", "native"},
    {"native_empty_edge", "", "exit", "semaphore_before_wait", "native"},
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
    if (event == "ring_wakeup_error_report") {
        ++s_reset_reports;
        if (s_reset_reader->posting_owner() != 0 || s_reset_reader->reader_slot_owner() != 0 ||
            s_reset_reader->inspection_holder() != 0)
        {
            s_reset_report_with_gate = true;
        }
    }
    if (event == "ring_reset_before_backend") {
        const auto attempt = ++s_reset_attempts;
        // The current registration remains until its checked drain succeeds.
        // Attempt two is still cleanup of wait one, before wait two registers.
        const auto preceding_wait = attempt == 3 ? 2u : 1u;
        if (s_reset_reader->pending_count_during_reset() != 1 || s_reset_waits != preceding_wait) {
            fixture_failure("checked drain lost its current registration or followed a new wait");
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
    require(reader.pending_count() == 1 && reader.token_present(),
        "failed same-reader cleanup did not retain its registration, token and reset obligation");
    std::printf("same-reader reset debt: registration retained, token present, attempts=%u\n",
        s_reset_attempts.load());

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
    require(s_reset_reports == 1 && !s_reset_report_with_gate,
        "same-reader cleanup reported a reset error while a shared gate was held");
    std::printf("same-reader checked drain: attempts=%u registration empty, token empty\n",
        s_reset_attempts.load());
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
    if (close) {
        require(readers.front()->pending_count() == 0,
            "mandatory close retry returned without completing its registered native wake");
    }
    else {
        require(readers.front()->pending_count() != 0, "failed notification lost its registration");
    }
    s_reader_released = true;
    if (count == 2) {
        require(await([&] { return completed == 1; }), "failed post prevented an unaffected reader wake");
    }
    if (!stop && !close) {
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
    const bool publication = std::string_view(test.action) == "publication";
    if (publication) {
        std::ofstream capture(s_directory / "poster_instance");
        capture << sintra::detail::current_process_instance();
        capture.close();
        require(bool(capture), "cannot arm writer identity before its constructor");
        s_poster_stage = test.poster_stage;
        sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
        sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void* object) {
            if (object == s_ownership_mutex) { wakeup_hook(stage, object); }
        };
        signal_file(s_directory / "poster_ready");
        require(await([] { return fs::exists(s_directory / "construct_writer"); }),
            "pre-constructor writer was not admitted");
    }
    {
        Writer writer(directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>());
        {
            std::ofstream instance(s_directory / "poster_instance");
            instance << sintra::detail::current_process_instance();
            instance.close();
            require(bool(instance), "cannot publish captured writer lock instance");
        }
        signal_file(s_directory / "poster_ready");
        require(await([] { return fs::exists(s_directory / "start_post"); }),
            "poster did not receive its start phase");
        s_poster_stage = test.poster_stage;
        sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
        if (std::string_view(test.action) == "request") {
            sintra::detail::test_hooks::s_ring_guard_operation =
                [](const char* stage, const std::atomic<uint64_t>*, uint8_t) { wakeup_hook(stage, nullptr); };
            const auto count = sintra::test::pick_ring_elements<uint32_t>();
            for (size_t i = 0; i <= count; ++i) {
                writer.write_commit(k_payload);
            }
        }
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

uint64_t captured_instance(const fs::path& path, const Exact_child& child)
{
    uint64_t instance = 0;
    std::ifstream capture(path);
    capture >> instance;
    require(bool(capture) && sintra::detail::process_instance_pid(instance) == uint32_t(child.pid()),
        "native fixture did not capture the exact child's lock instance");
    return instance;
}

using Native_record = sintra::detail::ring_native_notification_record_t;

std::shared_ptr<Native_record> map_native_record(const fs::path& directory,
    const char* filename = "native_record")
{
    sintra::ipc::file_mapping file(directory / filename, sintra::ipc::read_write);
    require(file.size() == sizeof(Native_record), "native sidecar fixture size mismatch");
    auto mapping = std::make_shared<sintra::ipc::mapped_region>(file, sintra::ipc::read_write, 0, 0);
    return std::shared_ptr<Native_record>(mapping, static_cast<Native_record*>(mapping->data()));
}

class Published_owner_witness : public sintra::detail::Native_exit_witness
{
public:
    explicit Published_owner_witness(std::shared_ptr<Native_record> record)

    :
        Native_exit_witness(record->writer_instance),
        m_record(std::move(record))
    {}

    bool has_exited() const noexcept override { return m_record->native_exit.load(); }

private:
    // The surviving parent holds the unreaped child/original Windows handle.
    // Only that native owner publishes this fact before admitting its actor.
    const std::shared_ptr<Native_record> m_record;
};

int run_recoverer(const wakeup_case_t& test, const char* directory, bool second = false)
{
    s_directory = directory;
    s_recoverer = true;
    if (second) { s_recoverer_label = "recoverer2"; }
    auto record = map_native_record(directory);
    auto witness = std::make_shared<Published_owner_witness>(record);
    std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>> witnesses{witness};
    if (second) {
        witnesses.push_back(std::make_shared<Published_owner_witness>(
            map_native_record(directory, "recoverer_record")));
    }
    auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
        std::move(witnesses));
    auto notification = std::make_shared<Native_notification>(
        directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), record, authority);
    {
        std::ofstream capture(s_directory / (s_recoverer_label + "_instance"));
        capture << sintra::detail::current_process_instance();
        capture.close();
        require(bool(capture), "cannot publish recoverer's exact lock word");
    }
    signal_file(s_directory / (s_recoverer_label + "_ready"));
    require(await([] { return fs::exists(s_directory / (s_recoverer_label + "_start")); }),
        "recoverer was not admitted");
    s_poster_stage = test.poster_stage;
    sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
    sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void* object) {
        if (std::string_view(stage) == "mutex_inspection_gate_acquired") { wakeup_hook(stage, object); }
    };
    require(notification->replay(1).state == Native_notification::Replay_state::COMPLETE,
        "recoverer failed before its controlled seam");
    throw std::runtime_error("recoverer missed the requested death seam");
}

int run_recoverer_scenario(const std::string& binary, const wakeup_case_t& test, const char* directory)
{
    s_directory = directory;
    sintra::detail::test_hooks::s_ring_wait_watchdog_enabled = false;
    sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
    sintra::detail::test_hooks::s_ring_wait_prepared = reader_prepared;
    auto poster = std::make_shared<Exact_child>(std::chrono::seconds(3));
    const bool publication = std::string_view(test.action) == "publication_recoverer";
    const char* writer_case = publication ? "native_owner_after_cas" : "native_parked_replay";
    const char* writer_args[] = {binary.c_str(), "--poster", writer_case, directory, nullptr};
    require(poster->spawn(binary.c_str(), writer_args), "cannot spawn retained writer");
    require(await([] { return fs::exists(s_directory / "poster_ready"); }), "writer was not armed");
    const auto writer_instance = captured_instance(s_directory / "poster_instance", *poster);
    {
        std::ofstream file(s_directory / "native_record", std::ios::binary);
        const std::vector<char> storage(sizeof(Native_record), 0);
        file.write(storage.data(), std::streamsize(storage.size()));
        file.close();
        require(bool(file), "cannot allocate native sidecar fixture");
    }
    auto record = map_native_record(directory);
    new (record.get()) Native_record(1, writer_instance);
    auto recoverer = std::make_shared<Exact_child>(std::chrono::seconds(3));
    const char* recovery_args[] = {binary.c_str(), "--recoverer", test.name, directory, nullptr};
    require(recoverer->spawn(binary.c_str(), recovery_args), "cannot spawn retained recoverer");
    require(await([] { return fs::exists(s_directory / "recoverer_ready"); }), "recoverer was not armed");
    const auto recoverer_instance = captured_instance(s_directory / "recoverer_instance", *recoverer);
    std::shared_ptr<Exact_child> second;
    std::shared_ptr<Native_record> recoverer_record;
    if (publication) {
        std::ofstream file(s_directory / "recoverer_record", std::ios::binary);
        const std::vector<char> storage(sizeof(Native_record), 0);
        file.write(storage.data(), std::streamsize(storage.size()));
        file.close();
        require(bool(file), "cannot allocate retained recoverer sidecar");
        recoverer_record = map_native_record(directory, "recoverer_record");
        new (recoverer_record.get()) Native_record(2, recoverer_instance);
        second = std::make_shared<Exact_child>(std::chrono::seconds(3));
        const char* args[] = {binary.c_str(), "--recoverer", test.name, directory, "second", nullptr};
        require(second->spawn(binary.c_str(), args), "cannot arm second retained recoverer");
        require(await([] { return fs::exists(s_directory / "recoverer2_ready"); }),
            "second recoverer was not armed");
    }
    std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>> witnesses{
        std::make_shared<Armed_child_witness>(writer_instance, poster),
        std::make_shared<Armed_child_witness>(recoverer_instance, recoverer)};
    if (second) {
        witnesses.push_back(std::make_shared<Armed_child_witness>(
            captured_instance(s_directory / "recoverer2_instance", *second), second));
    }
    auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
        std::move(witnesses));
    auto notification = std::make_shared<Native_notification>(
        directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), record, authority);
    auto reader = std::make_shared<Probe_reader>(
        directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), 0,
        sintra::detail::ring_directory_policy::caller_directory, notification);
    reader->start_reading();
    std::atomic<bool> valid{false};
    std::thread waiter([&] {
        tl_reader = true;
        const auto range = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
        valid = !reader->is_stopping() && (publication ? range.begin == range.end :
            range.end - range.begin == 1 && *range.begin == k_payload);
    });
    const auto fail = [&](const char* message) {
        std::fprintf(stderr, "%s: %s\n", test.name, message);
        std::string diagnostic;
        if (second) { (void)second->terminate_and_settle(diagnostic); }
        (void)recoverer->terminate_and_settle(diagnostic);
        (void)poster->terminate_and_settle(diagnostic);
        std::_Exit(2);
    };
    if (!await(native_reader_is_parked)) { fail("reader was not natively parked before writer death"); }
    std::printf("%s: native wait confirmed before writer/recoverer death\n", test.name);
    signal_file(s_directory / (publication ? "construct_writer" : "start_post"));
    if (!await([] { return fs::exists(s_directory / "poster_seam"); }) ||
        !poster->terminate_retaining_authority() ||
        !await([&] { return poster->observe_exit_retained() == Exact_child_state::exited; }))
    {
        fail("cannot retain exact writer death authority");
    }
    require(notification->observe_exit(), "native owner did not publish its retained death fact");
    signal_file(s_directory / "recoverer_start");
    if (!await([] { return fs::exists(s_directory / "recoverer_seam"); }) ||
        !recoverer->terminate_retaining_authority() ||
        !await([&] { return recoverer->observe_exit_retained() == Exact_child_state::exited; }))
    {
        fail("cannot retain exact recoverer death authority");
    }
    if (second) {
        recoverer_record->native_exit.store(true);
        signal_file(s_directory / "recoverer2_start");
        if (!await([] { return fs::exists(s_directory / "recoverer2_seam"); }) ||
            !second->terminate_retaining_authority() ||
            !await([&] { return second->observe_exit_retained() == Exact_child_state::exited; }))
        {
            fail("cannot retain second exact gate-recoverer death authority");
        }
        std::printf("%s: two exact inspection-gate recoverers terminated with witnesses retained\n", test.name);
        std::fflush(stdout);
    }
    require(notification->pending(), "interrupted recoverer discarded its unfinished obligation");
    const auto replay = notification->replay(1);
    if (publication && replay.state == Native_notification::Replay_state::REJECTED) {
        fail("partial mutex publication rejected exact native replay after two gate-recoverer deaths");
    }
    require(replay.state == Native_notification::Replay_state::COMPLETE &&
        !notification->pending(), "surviving owner could not repeat interrupted native replay");
    waiter.join();
    require(valid && !reader->consume_eviction_notification(), "recoverer death damaged committed delivery");
    reader->request_stop();
    reader->done_reading();
    require(poster->observe_exit_retained() == Exact_child_state::exited &&
        recoverer->observe_exit_retained() == Exact_child_state::exited &&
        (!second || second->observe_exit_retained() == Exact_child_state::exited),
        "native authority was released before replay and cleanup completed");
    return 0;
}

int run_request_scenario(const std::string& binary, const wakeup_case_t& test,
    const char* directory, bool recovery_enabled)
{
    s_directory = directory;
    auto poster = std::make_shared<Exact_child>(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), "--poster", test.name, directory, nullptr};
    require(poster->spawn(binary.c_str(), args), "cannot spawn REQUEST writer");
    require(await([] { return fs::exists(s_directory / "poster_ready"); }), "REQUEST writer was not armed");
    const auto instance = captured_instance(s_directory / "poster_instance", *poster);
    auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
        std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>>{
            std::make_shared<Armed_child_witness>(instance, poster)});
    auto record = std::make_shared<Native_record>(1, instance);
    auto notification = std::make_shared<Native_notification>(
        directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), record, authority);
    Probe_reader reader(directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), 0,
        sintra::detail::ring_directory_policy::caller_directory, notification);
    reader.start_reading();
    signal_file(s_directory / "start_post");
    require(await([] { return fs::exists(s_directory / "poster_seam"); }), "writer did not publish REQUEST");
    require(poster->terminate_retaining_authority() &&
        await([&] { return poster->observe_exit_retained() == Exact_child_state::exited; }),
        "cannot retain exact REQUEST writer death");
    const auto head = reader.get_leading_sequence();
    const auto counts = reader.guard_counts();
    // prepare_write arbitrates at the reserved end before publishing the last
    // element of the lap. The first element remains intact behind our guard.
    require(head == sintra::test::pick_ring_elements<uint32_t>() - 1,
        "REQUEST seam changed the committed head: " + std::to_string(head));
    uint32_t owned = 0;
    require(!reader.copy_committed_first(owned) && owned == 0,
        "REQUEST did not exclude extraction before recovery");
    if (recovery_enabled) {
        require(notification->replay(1).state == Native_notification::Replay_state::COMPLETE,
            "native owner could not settle dead REQUEST arbitration");
    }
    require(reader.copy_committed_first(owned) && owned == k_payload,
        "dead REQUEST stranded an intact committed frame");
    require(reader.get_leading_sequence() == head && reader.guard_counts() == counts &&
        !reader.consume_eviction_notification() && !reader.is_stopping(),
        "REQUEST cleanup changed committed data, guard ownership or loss state");
    reader.done_reading();
    return 0;
}

class Self_live_witness : public sintra::detail::Native_exit_witness
{
public:
    Self_live_witness() : Native_exit_witness(sintra::detail::current_process_instance()) {}
    bool has_exited() const noexcept override { return false; }
};

struct Withheld_proof
{
    std::atomic<bool> released{false};
    std::atomic<unsigned> attempts{0};
};

class Withheld_child_witness : public sintra::detail::Native_exit_witness
{
public:
    Withheld_child_witness(uint64_t instance, std::shared_ptr<Exact_child> child,
        std::shared_ptr<Withheld_proof> proof)
    : Native_exit_witness(instance), m_child(std::move(child)), m_proof(std::move(proof)) {}

    bool has_exited() const noexcept override
    {
        ++m_proof->attempts;
        return m_proof->released && m_child->observe_exit_retained() == Exact_child_state::exited;
    }
private:
    const std::shared_ptr<Exact_child> m_child;
    const std::shared_ptr<Withheld_proof> m_proof;
};

int s_exhausted_first_slot = -1;

int run_exhausted_reader_owner(const char* directory)
{
    const fs::path path(directory);
    {
        std::ofstream capture(path / "reader_owner_instance");
        capture << sintra::detail::current_process_instance() << '\n';
        require(bool(capture), "cannot publish exact reader owner's instance");
    }
    signal_file(path / "reader_owner_born");
    require(await([&] { return fs::exists(path / "reader_owner_enroll"); }),
        "reader owner did not receive preparticipation authority acknowledgement");
    std::vector<std::unique_ptr<Probe_reader>> readers;
    sintra::detail::test_hooks::s_ring_guard_operation = [](const char* stage, const std::atomic<uint64_t>*, uint8_t index) {
        if (s_exhausted_first_slot < 0 && std::string_view(stage) == "slot_acquired") { s_exhausted_first_slot = index; }
    };
    for (int i = 0; i < sintra::max_process_index; ++i) {
        auto reader = std::make_unique<Probe_reader>(directory, k_ring_name,
            sintra::test::pick_ring_elements<uint32_t>());
        reader->start_reading();
        readers.push_back(std::move(reader));
    }
    sintra::detail::test_hooks::s_ring_guard_operation = nullptr;
    require(s_exhausted_first_slot >= 0, "actual reader admission did not publish its slot observation");
    readers.front()->seed_registration(s_exhausted_first_slot);
    {
        std::ofstream capture(path / "reader_owner_slot");
        capture << s_exhausted_first_slot << '\n';
        require(bool(capture), "cannot publish registered reader slot");
    }
    signal_file(path / "reader_owner_full");
    require(await([&] { return fs::exists(path / "reader_owner_hold"); }),
        "reader owner did not receive seeded-token acknowledgement");
    readers.front()->posting_lock().lock();
    signal_file(path / "reader_owner_locked");
    // The original owner leaves every reader guard/slot and the posting lock
    // published. The fixture retains its native authority before this exit.
    std::_Exit(0);
}

std::atomic<bool> s_exhausted_reset_fault{false};
std::atomic<unsigned> s_exhausted_reset_attempts{0};

int run_exhausted_admission_case(const std::string& binary, const char* directory)
{
    const fs::path path(directory);
    const auto elements = sintra::test::pick_ring_elements<uint32_t>();
    Probe_writer writer(directory, k_ring_name, elements);
    s_reset_report_writer = &writer;
    auto child = std::make_shared<Exact_child>(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), "--exhausted-reader-owner", directory, nullptr};
    require(child->spawn(binary.c_str(), args), "cannot arm exact reader owner: " + child->error());
    require(await([&] { return fs::exists(path / "reader_owner_born"); }), "reader owner did not publish birth");
    uint64_t instance = 0;
    std::ifstream(path / "reader_owner_instance") >> instance;
    require(sintra::detail::process_instance_pid(instance) == uint32_t(child->pid()),
        "reader owner instance does not match retained native authority");
    auto proof = std::make_shared<Withheld_proof>();
    auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
        std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>>{
            std::make_shared<Self_live_witness>(), std::make_shared<Withheld_child_witness>(instance, child, proof)});
    signal_file(path / "reader_owner_enroll");
    require(await([&] { return fs::exists(path / "reader_owner_full"); }), "reader owner did not exhaust the actual slots");
    int registered = -1;
    std::ifstream(path / "reader_owner_slot") >> registered;
    require(registered >= 0 && writer.free_slots() == 0 && writer.pending_count_unlocked() == 1,
        "exhausted fixture did not publish every actual admission and registration");
    // Keep the Windows named token pinned in the surviving process too.
    writer.post_token(registered);
    signal_file(path / "reader_owner_hold");
    require(await([&] { return child->observe_exit_retained() == Exact_child_state::exited; }) &&
        fs::exists(path / "reader_owner_locked") && writer.posting_owner() == instance,
        "reader owner did not die while holding its published posting lock");
    const auto guards = writer.guard_counts();
    require(guards != 0, "dead actual snapshots did not retain their guard counts");
    // Construct after death: dead lifecycle attachments can clear without
    // exceeding the anchor's writer plus maximum-reader capacity before exit.
    auto notification = std::make_shared<Native_notification>(directory, k_ring_name, elements,
        std::make_shared<Native_record>(1, sintra::detail::current_process_instance()), authority);
    s_exhausted_reset_fault = true;
    sintra::detail::test_hooks::s_ipc_wakeup_operation = [](const char* stage, const void*) {
        observe_reset_report(stage);
        if (std::string_view(stage) == "ring_reset_before_backend") {
            ++s_exhausted_reset_attempts;
            if (s_exhausted_reset_fault) { sintra::detail::test_hooks::s_ipc_reset_error = EIO; }
        }
    };
    std::atomic<bool> finished{false};
    std::atomic<bool> rejected{false};
    std::thread admission([&] {
        try {
            Probe_reader reader(directory, k_ring_name, elements, 0,
                sintra::detail::ring_directory_policy::caller_directory, notification);
        } catch (const sintra::ring_acquisition_failure_exception&) { rejected = true; }
        finished = true;
    });
    if (!await([&] { return proof->attempts.load() != 0; })) {
        fixture_failure("nested scavenging did not consult the withheld exact posting-holder proof");
    }
    require(!finished && s_exhausted_reset_attempts == 0 && writer.free_slots_unlocked() == 0 &&
        writer.guard_counts() == guards && writer.posting_owner() == instance,
        "bound admission reclaimed a dead slot before exact nested-lock proof was available");
    proof->released = true;
    admission.join();
    require(rejected && finished && s_exhausted_reset_attempts == unsigned(sintra::max_process_index) &&
        writer.free_slots() == 0 && writer.guard_counts() == guards && writer.pending_count_unlocked() == 1,
        "failed checked scavenging reset retired or reused a guard/token-bearing slot");
    require(s_reset_reports == unsigned(sintra::max_process_index) && !s_reset_report_with_gate,
        "reader scavenging reported a reset error while a shared gate was held");
    require(writer.consume_token(registered), "failed checked reset erased the original native token");
    writer.post_token(registered);
    s_exhausted_reset_fault = false;
    {
        Probe_reader reader(directory, k_ring_name, elements, 0,
            sintra::detail::ring_directory_policy::caller_directory, notification);
        require(writer.free_slots() == sintra::max_process_index - 1 && writer.guard_counts() == 0 &&
            writer.pending_count_unlocked() == 0 && !writer.consume_token(registered),
            "successful checked drain did not settle dead registrations before ACTIVE reuse");
        reader.start_reading();
        require(writer.guard_counts() != 0, "reused reader slot could not acquire an actual snapshot");
        reader.done_reading();
    }
    sintra::detail::test_hooks::s_ipc_wakeup_operation = nullptr;
    require(writer.free_slots() == sintra::max_process_index && child->observe_exit_retained() == Exact_child_state::exited,
        "reused admission or repeated exact native observation lost retained custody");
    return 0;
}

#if !defined(_WIN32)
int run_post_fork_callback()
{
    unsigned callbacks = 0;
    sintra::set_log_callback([](sintra::log_level, const char* message, void* counter) {
        ++*static_cast<unsigned*>(counter);
        std::fprintf(stderr, "post-exec ordinary callback: %s", message);
    }, &callbacks);
    sintra::Log_stream(sintra::log_level::info) << "normal application behavior restored after exec\n";
    sintra::set_log_callback(nullptr);
    require(callbacks == 1, "ordinary logging callback did not run after exec");
    return 0;
}

int run_fork_exec_case(const std::string& binary, const char* directory)
{
    const auto elements = sintra::test::pick_ring_elements<uint32_t>();
    Probe_writer writer(directory, k_ring_name, elements);
    const auto instance = sintra::detail::current_process_instance();
    auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
        std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>>{std::make_shared<Self_live_witness>()});
    Probe_notification notification(directory, k_ring_name, elements, std::make_shared<Native_record>(1, instance), authority);
    int channel[2];
    require(::pipe(channel) == 0 && ::fcntl(channel[0], F_SETFD, FD_CLOEXEC) == 0 &&
        ::fcntl(channel[1], F_SETFD, FD_CLOEXEC) == 0, "cannot prepare fork-safe acknowledgement pipe");
    const char* program = binary.c_str();
    char* args[] = {const_cast<char*>(program), const_cast<char*>("--post-fork-callback"), nullptr};
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread logger([&] {
        std::unique_lock<std::mutex> lock(sintra::detail::log_mutex());
        held = true;
        while (!release.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    });
    require(await([&] { return held.load(); }), "ordinary logger was not held before multithreaded fork");
    pid_t child = -1;
    char acknowledgement = 0;
    sintra::detail::interprocess_mutex::Owner_inspection inspection;
    {
        sintra::spinlock::locker readers(notification.reader_lock());
        inspection = notification.ownership_mutex().inspect_owner_instance(instance, *authority, [&] {
            child = ::fork();
            if (child > 0) {
                ssize_t count;
                do { count = ::read(channel[0], &acknowledgement, 1); } while (count < 0 && errno == EINTR);
                if (count != 1) { acknowledgement = 0; }
            }
        });
        // Only PID-aware native guard release runs in the child on this path.
        // No Ring destruction or application callback is attempted before exec.
    }
    if (child == 0) {
        const bool parent_custody = notification.gate_holder() == instance &&
            notification.reader_lock().test_owner() == instance;
        sintra::detail::native_diagnostic("fork child native output; parent shared custody preserved=", parent_custody, "\n");
        const char ready = parent_custody ? 1 : 0;
        ssize_t written;
        do { written = ::write(channel[1], &ready, 1); } while (written < 0 && errno == EINTR);
        if (!parent_custody || written != 1) { ::_exit(81); }
        ::execv(program, args);
        ::_exit(82);
    }
    release = true;
    logger.join();
    ::close(channel[0]);
    ::close(channel[1]);
    require(child > 0 && acknowledgement == 1 &&
        inspection == sintra::detail::interprocess_mutex::Owner_inspection::MATCHED &&
        notification.gate_holder() == 0 && notification.reader_lock().test_owner() == 0,
        "fork-child guard abandonment modified the parent's actual shared custody");
    int status = 0;
    require(await([&] { return ::waitpid(child, &status, WNOHANG) == child; }) &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork/exec child did not restore ordinary callback behavior");
    writer.write_commit(k_payload);
    require(writer.get_leading_sequence() == 1, "parent writer lost ownership after child-image abandonment");
    return 0;
}
#endif

struct Recursion_probe
{
    const void* mutex;
    std::atomic<bool> snapshot_loaded{false};
    std::atomic<bool> other_owns{false};
};
Recursion_probe* s_recursion_probe = nullptr;

int run_recursion_case()
{
    using Mutex = sintra::detail::interprocess_mutex;
    const auto instance = sintra::detail::current_process_instance();
    const auto pid = uint32_t(sintra::get_current_pid());
    const auto tid = uint32_t(sintra::get_current_tid());
    const auto earlier_token = uint32_t(instance) + 1u;
    const auto earlier_instance = (uint64_t(pid) << 32u) | earlier_token;
    Mutex mutex;
    mutex.test_install_owner_fixture({pid, tid, 0, earlier_token});
    const auto stale_owner = mutex.test_owner_token();
    auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
        std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>>{
            std::make_shared<Self_live_witness>()});
    Recursion_probe probe{&mutex};
    s_recursion_probe = &probe;
    sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void* object) {
        if (object == s_recursion_probe->mutex &&
            std::string_view(stage) == "mutex_recursion_after_owner_load")
        {
            s_recursion_probe->snapshot_loaded = true;
            require(await([] { return s_recursion_probe->other_owns.load(); }),
                "other thread did not acquire after the stale recursion snapshot");
        }
    };
    std::atomic<bool> held{false};
    std::atomic<bool> attempted{false};
    std::atomic<bool> release{false};
    std::thread other([&] {
        require(mutex.inspect_owner_instance(earlier_instance, *authority, [&] {
            held = true;
            require(await([&] { return probe.snapshot_loaded.load() || attempted.load(); }),
                "contender did not enter the recursion decision");
        }) == Mutex::Owner_inspection::MATCHED, "predecessor inspection did not hold its gate");
        mutex.lock();
        probe.other_owns = true;
        require(await([&] { return release.load(); }), "contender did not release the fixture");
        mutex.unlock();
    });
    require(await([&] { return held.load(); }), "inspection gate was not held by the other thread");
    bool false_recursion = false;
    bool acquired = false;
    try { acquired = mutex.test_try_lock_throwing(); }
    catch (const std::system_error& error) {
        require(error.code() == std::make_error_code(std::errc::resource_deadlock_would_occur),
            "recursion attempt threw an unrelated error");
        false_recursion = true;
    }
    attempted = true;
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

int run_gate_exception_case(const wakeup_case_t& test)
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

int run_timed_exception_case(const wakeup_case_t& test)
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

Probe_writer* s_retirement_writer = nullptr;
Probe_reader* s_close_reader = nullptr;
const char* s_close_probe_stage = "ring_close_published";
std::atomic<int> s_retirement_index{-1};
std::atomic<unsigned> s_retirement_resets{0};
bool s_close_backend_retry = false;
std::atomic<unsigned> s_close_post_attempts{0};
std::atomic<bool> s_reset_fault_held{true};

void retirement_termination()
{
    if (s_close_reader) {
        std::fprintf(stderr, "writer close termination before cleanup: owner=%llu posting_owner=%llu registrations=%d\n",
            static_cast<unsigned long long>(s_close_reader->writer_owner()),
            static_cast<unsigned long long>(s_close_reader->posting_owner()),
            s_close_reader->pending_count_unlocked());
    } else {
        std::fprintf(stderr, "reader release termination before cleanup: read_access=%llu free_slots=%d posting_owner=%llu\n",
            static_cast<unsigned long long>(s_retirement_writer->guard_counts()),
            s_retirement_writer->free_slots_unlocked(),
            static_cast<unsigned long long>(s_retirement_writer->posting_owner()));
    }
    std::fflush(stderr);
    std::_Exit(79);
}

Probe_reader* s_close_error_reader = nullptr;
int s_close_error_indices[2]{-1, -1};
unsigned s_close_error_admissions = 0;
unsigned s_close_error_posts = 0;
unsigned s_close_error_reports = 0;
bool s_close_error_gate_held = false;
bool s_close_error_duty_lost = false;

int run_close_error_case(const char* directory)
{
    namespace hooks = sintra::detail::test_hooks;
    const auto elements = sintra::test::pick_ring_elements<uint32_t>();
    auto writer = std::make_unique<Probe_writer>(directory, k_ring_name, elements);
    hooks::s_ring_guard_operation = [](const char* stage, const std::atomic<uint64_t>*, uint8_t index) {
        if (std::string_view(stage) == "slot_acquired" && s_close_error_admissions < 2) {
            s_close_error_indices[s_close_error_admissions++] = index;
        }
    };
    Probe_reader first(directory, k_ring_name, elements);
    Probe_reader second(directory, k_ring_name, elements);
    hooks::s_ring_guard_operation = nullptr;
    require(s_close_error_admissions == 2 && s_close_error_indices[0] != s_close_error_indices[1],
        "close error fixture did not acquire two distinct lifetime slots");
    first.seed_registration(s_close_error_indices[0]);
    second.seed_registration(s_close_error_indices[1]);
    s_close_error_reader = &first;
    hooks::s_ipc_wakeup_operation = [](const char* stage, const void*) {
        const std::string_view event = stage;
        if (event == "ring_post_before_backend") {
            const auto round = s_close_error_posts / 2;
            const auto position = s_close_error_posts++ % 2;
            if (s_close_error_reader->writer_owner() == 0 ||
                s_close_error_reader->pending_count_unlocked() != 2)
            {
                s_close_error_duty_lost = true;
            }
            // Reverse-stack position zero is the first failed entry every time.
            // Its unchanged EIO must not hide position one's new EACCES.
            if (round < 3) {
                hooks::s_ipc_binary_post_error = position == 0 || round == 0 ? EIO : EACCES;
            }
        }
        if (event == "ring_wakeup_error_report") {
            ++s_close_error_reports;
            if (s_close_error_reader->posting_owner() != 0 ||
                s_close_error_reader->reader_slot_owner() != 0 ||
                s_close_error_reader->inspection_holder() != 0)
            {
                s_close_error_gate_held = true;
            }
            if (s_close_error_reader->writer_owner() == 0 ||
                s_close_error_reader->pending_count_unlocked() != 2)
            {
                s_close_error_duty_lost = true;
            }
        }
    };
    writer.reset();
    hooks::s_ipc_wakeup_operation = nullptr;
    const bool tokens = first.token_signaled_at(s_close_error_indices[0]) &&
        second.token_signaled_at(s_close_error_indices[1]);
    std::printf("multiple-slot close: posts=%u reports=%u shared_gate_held=%d duty_lost=%d tokens=%d\n",
        s_close_error_posts, s_close_error_reports, int(s_close_error_gate_held),
        int(s_close_error_duty_lost), int(tokens));
    require(s_close_error_posts == 8 && tokens && first.writer_owner() == 0 && first.pending_count() == 0 &&
        !s_close_error_duty_lost, "multiple-slot close lost retry custody or checked completion");
    require(s_close_error_reports == 3 && !s_close_error_gate_held,
        "unchanged first-slot error suppressed another slot's changed close error");
    return 0;
}

int run_retirement_case(const wakeup_case_t& test, const char* directory)
{
    namespace hooks = sintra::detail::test_hooks;
    const std::string_view name = test.name;
    hooks::s_ring_wait_watchdog_enabled = false;
    auto writer = std::make_unique<Probe_writer>(directory, k_ring_name,
        sintra::test::pick_ring_elements<uint32_t>());
    const int free_before = writer->free_slots();
    hooks::s_ring_guard_operation = [](const char* stage, const std::atomic<uint64_t>*, uint8_t index) {
        if (std::string_view(stage) == "slot_acquired") { s_retirement_index = index; }
    };
    auto reader = std::make_unique<Probe_reader>(directory, k_ring_name,
        sintra::test::pick_ring_elements<uint32_t>());
    hooks::s_ring_guard_operation = nullptr;
    const int index = s_retirement_index.load();
    require(index >= 0, "retirement fixture did not capture its reader slot");
    reader->start_reading();
    if (name == "reader_release_throw_cleanup") {
        require(writer->guard_counts() != 0, "reader release fixture has no active snapshot guard");
        s_retirement_writer = writer.get();
        std::set_terminate(retirement_termination);
        hooks::s_ring_guard_operation = [](const char* stage, const std::atomic<uint64_t>*, uint8_t) {
            if (std::string_view(stage) == "release") {
                s_probe_thrown = true;
                throw std::runtime_error("fixture active-snapshot release exception");
            }
        };
        reader.reset();
        hooks::s_ring_guard_operation = nullptr;
        require(s_probe_thrown && writer->guard_counts() == 0 && writer->free_slots() == free_before,
            "reader release probe prevented decrement or slot return");
        require(hooks::take_observation_failure() != nullptr,
            "snapshot release swallowed its instrumentation failure");
        return 0;
    }
    if (name == "writer_close_throw_cleanup" || name == "writer_close_flush_throw_cleanup" ||
        name == "writer_close_post_throw_cleanup" || name == "writer_close_count_throw_cleanup" ||
        name == "writer_close_backend_retry" || name == "reader_stop_backend_retry")
    {
        s_close_backend_retry = name == "writer_close_backend_retry" || name == "reader_stop_backend_retry";
        s_close_probe_stage = name == "writer_close_flush_throw_cleanup" ? "ring_flush_before_post" :
            name == "writer_close_post_throw_cleanup" ? "ring_post_before_backend" :
            name == "writer_close_count_throw_cleanup" ? "semaphore_count_published" :
            s_close_backend_retry ? "ring_post_before_backend" : "ring_close_published";
        hooks::s_ipc_wakeup_operation = wakeup_hook;
        hooks::s_ring_wait_prepared = reader_prepared;
        std::atomic<bool> completed{false};
        std::thread waiter([&] {
            tl_reader = true;
            const auto range = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
            const bool first_empty = range.begin == range.end;
            const bool first_stopping = reader->is_stopping();
            bool final_stopping = first_stopping;
            if (first_empty && !first_stopping) {
                // A native wake may return empty; the next call observes close.
                // No later writer operation or replay is needed for that call.
                const auto closed = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
                final_stopping = closed.begin == closed.end && reader->is_stopping();
            }
            std::printf("writer close reader: first_empty=%d first_stopping=%d final_stopping=%d\n",
                int(first_empty), int(first_stopping), int(final_stopping));
            completed = first_empty && final_stopping;
        });
        if (!await(native_reader_is_parked)) { fixture_failure("close fixture reader did not park natively"); }
        std::printf("writer close fixture: native wait confirmed\n");
        std::fflush(stdout);
        s_close_reader = reader.get();
        std::set_terminate(retirement_termination);
        hooks::s_ipc_wakeup_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == s_close_probe_stage) {
                if (s_close_backend_retry) {
                    if (++s_close_post_attempts == 1) {
                        sintra::detail::test_hooks::s_ipc_binary_post_error = EIO;
                    }
                    return;
                }
                std::fprintf(stderr, "writer close probe reached: stage=%s process_still_alive=1\n", stage);
                std::fflush(stderr);
                s_probe_thrown = true;
                throw std::runtime_error("fixture writer close exception");
            }
            wakeup_hook(stage, object);
        };
        if (name == "reader_stop_backend_retry") {
            reader->request_stop();
        }
        else {
            writer.reset();
        }
        hooks::s_ipc_wakeup_operation = wakeup_hook;
        waiter.join();
        const bool owner_preserved = name == "reader_stop_backend_retry"
            ? reader->writer_owner() != 0 : reader->writer_owner() == 0;
        require((s_close_backend_retry ? s_close_post_attempts.load() >= 2 : s_probe_thrown.load()) &&
            completed && owner_preserved && reader->posting_owner() == 0,
            "writer close probe skipped flush, wake or original ownership cleanup");
        if (!s_close_backend_retry) {
            require(hooks::take_observation_failure() != nullptr,
                "writer close swallowed its instrumentation failure");
        }
        std::printf("mandatory native wake: attempts=%u reader_stopping=%d\n",
            s_close_post_attempts.load(), int(reader->is_stopping()));
        return 0;
    }
    writer->post_token(index);
    s_reset_report_writer = writer.get();
    const bool persistent = name == "reader_retirement_persistent_reset";
    hooks::s_ipc_wakeup_operation = [](const char* stage, const void*) {
        observe_reset_report(stage);
        if (std::string_view(stage) == "ring_reset_before_backend") {
            const auto attempt = ++s_retirement_resets;
            const bool fail = s_reset_fault_held.load();
            if (attempt == 2 && fail) {
                while (s_reset_fault_held.load()) { std::this_thread::yield(); }
            }
            if (fail) { sintra::detail::test_hooks::s_ipc_reset_error = EIO; }
            else if (attempt == 3 || attempt == 4) {
                sintra::detail::test_hooks::s_ipc_reset_error = EACCES;
            }
        }
    };
    if (persistent) {
        std::atomic<bool> retired{false};
        std::thread retire([&] { reader.reset(); retired = true; });
        if (!await([&] { return retired.load() || s_retirement_resets.load() >= 2; })) {
            fixture_failure("retirement neither retained reset debt nor completed");
        }
        const bool retired_with_fault = retired.load();
        if (!retired_with_fault) {
            require(writer->free_slots_unlocked() == free_before - 1,
                "persistent reset failure returned a still-owned slot");
        }
        s_reset_fault_held = false;
        retire.join();
        hooks::s_ipc_wakeup_operation = nullptr;
        if (retired_with_fault) {
            std::fprintf(stderr, "persistent retirement: reset_attempts=%u stale_token=%d free_slots=%d\n",
                s_retirement_resets.load(), int(writer->consume_token(index)), writer->free_slots());
        }
        require(!retired_with_fault,
            "retirement completed while checked reset continued to fail; token debt lost");
    } else {
        // Exactly one injected reset failure; later attempts must succeed.
        hooks::s_ipc_wakeup_operation = [](const char* stage, const void*) {
            observe_reset_report(stage);
            if (std::string_view(stage) == "ring_reset_before_backend" && ++s_retirement_resets == 1) {
                sintra::detail::test_hooks::s_ipc_reset_error = EIO;
            }
        };
        reader.reset();
        hooks::s_ipc_wakeup_operation = nullptr;
    }
    const bool stale_token = writer->consume_token(index);
    std::printf("reader retirement: reset_attempts=%u stale_token=%d free_slots=%d\n",
        s_retirement_resets.load(), int(stale_token), writer->free_slots());
    require(s_retirement_resets >= 2 && !stale_token && writer->free_slots() == free_before,
        "reader retirement discarded checked reset debt and returned a stale-token slot");
    require(s_reset_reports == (persistent ? 2u : 1u) && !s_reset_report_with_gate,
        "reader retirement did not report only the first or changed error outside shared gates");
    std::printf("reader reset diagnostics: reports=%u shared_gate_held=%d\n",
        s_reset_reports.load(), int(s_reset_report_with_gate.load()));
    return 0;
}

int run_custody_case(const wakeup_case_t& test, const char* directory)
{
    namespace hooks = sintra::detail::test_hooks;
    const std::string_view name = test.name;
    const auto elements = sintra::test::pick_ring_elements<uint32_t>();
    s_throw_mutex_stage = test.poster_stage;
    s_ownership_mutex = nullptr;
    if (name == "reader_admission_reset_failure") {
        Probe_writer writer(directory, k_ring_name, elements);
        const int available = writer.free_slots();
        s_reset_report_writer = &writer;
        hooks::s_ipc_wakeup_operation = [](const char* stage, const void*) { observe_reset_report(stage); };
        hooks::s_ipc_reset_error = EIO;
        bool rejected = false;
        try {
            Probe_reader reader(directory, k_ring_name, elements);
        }
        catch (const std::system_error&) {
            rejected = true;
        }
        require(rejected && writer.free_slots() == available && writer.guard_counts() == 0,
            "failed admission leaked its reserved slot or published a guard");
        require(s_reset_reports == 1 && !s_reset_report_with_gate,
            "reader admission reported a reset error while a shared gate was held");
        hooks::s_ipc_wakeup_operation = nullptr;
        {
            Probe_reader reader(directory, k_ring_name, elements);
            require(writer.free_slots() == available - 1,
                "failed reservation was not available for checked subsequent admission");
        }
        require(writer.free_slots() == available, "subsequent admitted reader did not retire");
        return 0;
    }
    const bool lifecycle = name.starts_with("lifecycle_");
    if (lifecycle) {
        hooks::s_mutex_operation = [](const char* stage, const void*) {
            if (std::string_view(stage) == s_throw_mutex_stage) {
                s_probe_thrown = true;
                throw std::runtime_error("fixture lifecycle acquisition probe");
            }
        };
        {
            Probe_reader reader(directory, k_ring_name, elements);
        }
        hooks::s_mutex_operation = nullptr;
        require(s_probe_thrown && hooks::take_observation_failure() != nullptr,
            "lifecycle cleanup lost its deferred acquisition probe");
        require(!fs::exists(fs::path(directory) / k_ring_name) &&
            !fs::exists(fs::path(directory) / (std::string(k_ring_name) + "_control")),
            "lifecycle cleanup retained a completed ring attachment");
        Probe_writer replacement(directory, k_ring_name, elements);
        return 0;
    }
    Probe_reader reader(directory, k_ring_name, elements);
    hooks::s_ipc_wakeup_operation = wakeup_hook;
    hooks::s_mutex_operation = [](const char* stage, const void* mutex) {
        if (mutex == s_ownership_mutex && std::string_view(stage) == s_throw_mutex_stage) {
            s_probe_thrown = true;
            throw std::runtime_error("fixture writer constructor acquisition probe");
        }
    };
    {
        Probe_writer writer(directory, k_ring_name, elements);
        require(reader.writer_owner() != 0, "writer constructor lost its successful acquired token");
        hooks::s_mutex_operation = nullptr;
        reader.start_reading();
        writer.write_commit(k_payload);
    }
    hooks::s_ipc_wakeup_operation = nullptr;
    require(s_probe_thrown && hooks::take_observation_failure() != nullptr && reader.writer_owner() == 0,
        "writer constructor probe interrupted original ownership cleanup");
    uint32_t committed = 0;
    require(reader.copy_committed_first(committed) && committed == k_payload,
        "writer constructor cleanup damaged committed payload");
    return 0;
}

int run_capability_case(const char* directory)
{
    const auto instance = sintra::detail::current_process_instance();
    auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
        std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>>{
            std::make_shared<Self_live_witness>()});
    const auto reject = [&](std::shared_ptr<Native_record> record,
        std::shared_ptr<const sintra::detail::Native_exit_authority> proof) {
        bool rejected = false;
        try {
            Native_notification notification(
                directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), record, proof);
        }
        catch (const sintra::ring_acquisition_failure_exception&) { rejected = true; }
        require(rejected, "invalid optional native capability was admitted");
    };
    reject(nullptr, authority);
    reject(std::make_shared<Native_record>(1, instance), nullptr);
    reject(std::make_shared<Native_record>(1, instance, Native_record::k_version + 1), authority);
    reject(std::make_shared<Native_record>(0, instance), authority);
    reject(std::make_shared<Native_record>(1, instance ^ 1u), authority);
    auto notification = std::make_shared<Native_notification>(directory, k_ring_name,
        sintra::test::pick_ring_elements<uint32_t>(), std::make_shared<Native_record>(1, instance), authority);
    bool rejected = false;
    try {
        Probe_reader reader(directory, "another_mapping", sintra::test::pick_ring_elements<uint32_t>(), 0,
            sintra::detail::ring_directory_policy::caller_directory, notification);
    }
    catch (const sintra::ring_acquisition_failure_exception&) { rejected = true; }
    require(rejected && notification->replay(1).state == Native_notification::Replay_state::REJECTED &&
        !notification->exited(), "mapping mismatch or live native owner was accepted as death");
    return 0;
}

bool s_lifecycle_acquired = false;
int s_opened_directory_fd = -1;
const char* s_construction_stage = nullptr;
unsigned s_construction_faults = 0;
void* s_failed_control_address = nullptr;
size_t s_failed_control_size = 0;

void set_fixture_environment(const char* name, const std::string& value)
{
#if defined(_WIN32)
    require(_putenv_s(name, value.c_str()) == 0, "cannot set construction fixture environment");
#else
    require(::setenv(name, value.c_str(), 1) == 0, "cannot set construction fixture environment");
#endif
}
#if !defined(_WIN32)
void* s_reused_mappings[8]{};
size_t s_reused_mapping_count = 0;
size_t s_mapping_bytes = 0;
#endif

int run_construction_case(const wakeup_case_t& test, const char* directory)
{
    const std::string_view name = test.name;
    const std::string path(directory);
    const auto elements = sintra::test::pick_ring_elements<uint32_t>();
    bool failed = false;
    if (name == "data_first_mapping_allocation_unwind" || name == "control_mapping_allocation_unwind" ||
        name == "control_abi_diagnostic_allocation_unwind")
    {
        Probe_writer writer(path, k_ring_name, elements);
        const auto slots = writer.free_slots();
        const auto original_abi = name == "control_abi_diagnostic_allocation_unwind" ?
            writer.replace_control_abi(0) : 0;
        s_construction_stage = name == "data_first_mapping_allocation_unwind" ? "data_before_first_mapping" :
            name == "control_mapping_allocation_unwind" ? "control_before_mapping" : "control_abi_diagnostic";
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == s_construction_stage) {
                ++s_construction_faults;
                if (object) {
                    auto* region = static_cast<const sintra::detail::ipc::mapped_region*>(object);
                    s_failed_control_address = const_cast<void*>(region->data());
                    s_failed_control_size = region->size();
                }
                s_fail_next_allocation = true;
            }
        };
        try { Probe_reader reader(path, k_ring_name, elements); }
        catch (const std::exception&) { failed = true; }
        s_fail_next_allocation = false;
        sintra::detail::s_ring_construction_operation = nullptr;
        if (original_abi) { writer.replace_control_abi(original_abi); }
        require(failed && s_construction_faults == (name == "data_first_mapping_allocation_unwind" ? 8u : 1u),
            "actual construction allocation failure did not unwind the requested resource interval");
        if (s_failed_control_address) {
#if defined(_WIN32)
            MEMORY_BASIC_INFORMATION info{};
            require(VirtualQuery(s_failed_control_address, &info, sizeof(info)) == sizeof(info) &&
                info.State == MEM_FREE, "ABI diagnostic allocation leaked its control view");
#else
            require(::mprotect(s_failed_control_address, s_failed_control_size, PROT_READ | PROT_WRITE) == -1 &&
                errno == ENOMEM, "ABI diagnostic allocation leaked its control mapping");
#endif
        }
        Probe_reader replacement(path, k_ring_name, elements, 1);
        writer.write_commit(k_payload);
        const auto data = replacement.start_reading(1);
        require(data.end - data.begin == 1 && *data.begin == k_payload && writer.free_slots() == slots - 1,
            "constructor unwind damaged committed data or a later real admission");
        replacement.done_reading();
        return 0;
    }
    if (name == "data_attach_marker_allocation_cleanup" || name == "before_release_marker_allocation_cleanup" ||
        name == "after_release_marker_allocation_cleanup")
    {
        const auto data_file = (fs::path(path) / k_ring_name).string();
        const auto marker = (fs::path(path) / "marker_observed").string();
        const auto resume = (fs::path(path) / "marker_resume").string();
        signal_file(resume);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_PAUSE_DATA_FILE", data_file);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_PAUSED_FILE", marker);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RESUME_FILE", resume);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RELEASE_DATA_FILE", data_file);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RELEASE_WAITING_FILE", marker);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RELEASE_LOCKED_FILE", marker);
        s_construction_stage = name == "data_attach_marker_allocation_cleanup" ? "data_attach_marker" :
            name == "before_release_marker_allocation_cleanup" ? "before_release_marker" : "after_release_marker";
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void*) {
            if (std::string_view(stage) == s_construction_stage) { ++s_construction_faults; s_fail_next_allocation = true; }
        };
        { Probe_reader reader(path, k_ring_name, elements); reader.start_reading(); }
        sintra::detail::s_ring_construction_operation = nullptr;
        s_fail_next_allocation = false;
        require(s_construction_faults == 1 && sintra::detail::test_hooks::take_observation_failure() != nullptr,
            "whole marker boundary did not retain the actual allocation failure");
        require(!fs::exists(data_file) && !fs::exists(data_file + "_control"),
            "marker allocation cancelled mandatory final attachment cleanup");
        Probe_writer replacement(path, k_ring_name, elements);
        replacement.write_commit(k_payload);
        return 0;
    }
    if (name == "private_named_path_allocation_unwind") {
        sintra::detail::native_diagnostic("named path fixture: before construction\n", 0, "", false);
        sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void*) {
            if (std::string_view(stage) == "mutex_after_owner_cas") { s_lifecycle_acquired = true; }
        };
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void*) {
            if (s_lifecycle_acquired && std::string_view(stage) == "private_directory_named_path") {
                sintra::detail::native_diagnostic("named path fixture: arm actual allocation\n", 0, "", false);
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
                // MSVC's conversion default-constructs a noexcept wide string
                // with a Debug iterator proxy. Target its throwing resize instead.
                s_successful_allocations_before_failure = 1;
#endif
                s_fail_next_allocation = true;
            }
        };
        try {
            Probe_reader reader(path, k_ring_name, elements, 0,
                sintra::detail::ring_directory_policy::private_existing_directory);
        } catch (const std::exception&) {
            failed = true;
            sintra::detail::native_diagnostic("named path fixture: constructor unwound\n", 0, "", false);
        }
        s_fail_next_allocation = false;
        sintra::detail::s_ring_construction_operation = nullptr;
        sintra::detail::test_hooks::s_mutex_operation = nullptr;
        require(failed && s_lifecycle_acquired, "private path fault did not reach acquired lifecycle unwind");
        sintra::detail::native_diagnostic("named path fixture: before replacement\n", 0, "", false);
        Probe_writer replacement(path, k_ring_name, elements, sintra::detail::ring_directory_policy::private_existing_directory);
        sintra::detail::native_diagnostic("named path fixture: replacement acquired\n", 0, "", false);
        replacement.write_commit(k_payload);
        return 0;
    }
#if !defined(_WIN32)
#if defined(__linux__) && defined(MAP_SYNC) && defined(MAP_SHARED_VALIDATE) && defined(MAP_FIXED_NOREPLACE)
    if (name == "data_native_mapping_failure_reuse") {
        s_mapping_bytes = elements * sizeof(uint32_t);
        sintra::detail::s_ring_mapping_options_for_test = MAP_SHARED_VALIDATE | MAP_SYNC;
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == "data_mapping_native_failed") {
                require(int(reinterpret_cast<intptr_t>(object)) == EOPNOTSUPP,
                    "native mapping fixture did not reach the unsupported synchronous file mapper");
                ++s_construction_faults;
            }
            if (std::string_view(stage) == "data_mapping_before_retry") {
                require(s_reused_mapping_count < 8 && s_construction_faults == s_reused_mapping_count + 1,
                    "native mapping failure was not observed before address reuse");
                std::thread reuser([object] {
                    void* replacement = ::mmap(const_cast<void*>(object), s_mapping_bytes,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                    require(replacement == object, "failed native mapping did not leave the interval vacant");
                    *static_cast<uint32_t*>(replacement) = k_payload;
                    s_reused_mappings[s_reused_mapping_count++] = replacement;
                });
                reuser.join();
            }
        };
        try { Probe_reader reader(path, k_ring_name, elements); }
        catch (const sintra::ring_acquisition_failure_exception&) { failed = true; }
        sintra::detail::s_ring_construction_operation = nullptr;
        sintra::detail::s_ring_mapping_options_for_test = 0;
        bool preserved = failed && s_construction_faults == 8 && s_reused_mapping_count == 8;
        for (size_t i = 0; i < s_reused_mapping_count; ++i) {
            const bool mapped = ::mprotect(s_reused_mappings[i], s_mapping_bytes, PROT_READ | PROT_WRITE) == 0;
            preserved = preserved && mapped;
            if (mapped) {
                preserved = preserved && *static_cast<uint32_t*>(s_reused_mappings[i]) == k_payload;
                require(::munmap(s_reused_mappings[i], s_mapping_bytes) == 0, "native replacement unmap failed");
            }
        }
        require(preserved, "native mapping failure cleanup unmapped another thread's replacement");
        Probe_writer replacement(path, k_ring_name, elements);
        replacement.write_commit(k_payload);
        return 0;
    }
#endif
    if (name == "private_opened_fd_allocation_unwind") {
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == "private_directory_opened") {
                s_opened_directory_fd = int(reinterpret_cast<intptr_t>(object));
                s_fail_next_allocation = true;
            }
        };
        try {
            sintra::detail::ring_directory held(path,
                sintra::detail::ring_directory_policy::private_existing_directory);
        } catch (const std::bad_alloc&) { failed = true; }
        sintra::detail::s_ring_construction_operation = nullptr;
        s_fail_next_allocation = false;
        errno = 0;
        const int disposition = ::fcntl(s_opened_directory_fd, F_GETFD);
        require(failed && s_opened_directory_fd >= 0 && disposition == -1 && errno == EBADF,
            "failed directory construction leaked its already-open native descriptor");
        return 0;
    }
    if (name == "data_second_mapping_allocation_unwind") {
        s_mapping_bytes = elements * sizeof(uint32_t);
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == "data_first_mapping") {
                s_fail_next_allocation = true;
            }
            if (std::string_view(stage) == "data_mapping_before_retry") {
                require(s_reused_mapping_count < 8, "unexpected additional mapping attempt");
                // A separate thread obtains the released first interval while
                // the failure path still owns its remaining cleanup work.
                std::thread reuser([object] {
                    void* replacement = ::mmap(const_cast<void*>(object), s_mapping_bytes,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                    require(replacement == object, "could not reuse the released mapping interval");
                    *static_cast<uint32_t*>(replacement) = k_payload;
                    s_reused_mappings[s_reused_mapping_count++] = replacement;
                });
                reuser.join();
            }
        };
        try { Probe_reader reader(path, k_ring_name, elements); }
        catch (const sintra::ring_acquisition_failure_exception&) { failed = true; }
        sintra::detail::s_ring_construction_operation = nullptr;
        s_fail_next_allocation = false;
        bool preserved = failed && s_reused_mapping_count == 8;
        for (size_t i=0; i<s_reused_mapping_count; ++i) {
            const bool mapped = ::mprotect(s_reused_mappings[i], s_mapping_bytes, PROT_READ | PROT_WRITE) == 0;
            preserved = preserved && mapped;
            if (mapped) {
                preserved = preserved && *static_cast<uint32_t*>(s_reused_mappings[i]) == k_payload;
                require(::munmap(s_reused_mappings[i], s_mapping_bytes) == 0, "replacement unmap failed");
            }
        }
        require(preserved, "partial mapping cleanup unmapped a different thread's replacement interval");
        Probe_writer replacement(path, k_ring_name, elements);
        return 0;
    }
#endif
    throw std::runtime_error("unknown construction case");
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

int run_backoff_case(const wakeup_case_t& test)
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

int run_scenario(const std::string& binary, const wakeup_case_t& test, const char* directory,
    bool recovery_enabled = true)
{
    if (std::string_view(test.action) == "exhausted_admission") { return run_exhausted_admission_case(binary, directory); }
#if !defined(_WIN32)
    if (std::string_view(test.action) == "fork_exec") { return run_fork_exec_case(binary, directory); }
#endif
    if (std::string_view(test.action) == "recursion") { return run_recursion_case(); }
    if (std::string_view(test.action) == "gate_throw") { return run_gate_exception_case(test); }
    if (std::string_view(test.action) == "timed_throw") { return run_timed_exception_case(test); }
    if (std::string_view(test.action) == "close_errors") { return run_close_error_case(directory); }
    if (std::string_view(test.action) == "retirement") { return run_retirement_case(test, directory); }
    if (std::string_view(test.action) == "custody") { return run_custody_case(test, directory); }
    if (std::string_view(test.action) == "construction") { return run_construction_case(test, directory); }
#if !defined(_WIN32)
    if (std::string_view(test.action) == "acquisition_error") { return run_mandatory_error_case(); }
    if (std::string_view(test.action) == "backoff_error") { return run_backoff_case(test); }
#endif
    if (std::string_view(test.action) == "request") {
        return run_request_scenario(binary, test, directory, recovery_enabled);
    }
    if (std::string_view(test.action) == "recoverer" ||
        std::string_view(test.action) == "publication_recoverer") {
        return run_recoverer_scenario(binary, test, directory);
    }
    if (std::string_view(test.action) == "capability") {
        return run_capability_case(directory);
    }
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
    auto poster_owner = std::make_shared<Exact_child>(std::chrono::seconds(3));
    auto& poster = *poster_owner;
    const char* args[] = {binary.c_str(), "--poster", test.name, directory, nullptr};
    require(poster.spawn(binary.c_str(), args), "cannot spawn poster: " + poster.error());
    require(await([] { return fs::exists(s_directory / "poster_ready"); }),
        "poster did not acquire writer ownership");

    std::shared_ptr<Native_notification> notification;
    const bool native_case = std::string_view(test.retry) == "native";
    if (native_case) {
        uint64_t instance = 0;
        std::ifstream capture(s_directory / "poster_instance");
        capture >> instance;
        require(bool(capture) && sintra::detail::process_instance_pid(instance) == uint32_t(poster.pid()),
            "native fixture did not capture this exact child's writer instance");
        auto witness = std::make_shared<Armed_child_witness>(instance, poster_owner);
        auto authority = std::make_shared<sintra::detail::Native_exit_authority>(
            std::vector<std::shared_ptr<const sintra::detail::Native_exit_witness>>{witness});
        require(!authority->has_exited(instance ^ 1u), "native authority accepted another lock word");
        auto record = std::make_shared<sintra::detail::ring_native_notification_record_t>(1, instance);
        notification = std::make_shared<Probe_notification>(
            directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), record, authority);
    }
    auto reader = std::make_shared<Probe_reader>(
        directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), 0,
        sintra::detail::ring_directory_policy::caller_directory, notification);
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
    signal_file(s_directory / (std::string_view(test.action) == "publication" ? "construct_writer" : "start_post"));
    if (native_case && std::string_view(test.poster_stage).empty()) {
        if (!await([&] { return poster.observe_exit_retained() == Exact_child_state::exited; })) {
            fail("poster native exit was not observed with retained authority");
        }
    }
    else
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
    if (native_case) {
        if (!poster.terminate_retaining_authority() ||
            !await([&] { return poster.observe_exit_retained() == Exact_child_state::exited; }))
        {
            fail("cannot prove exact native exit while retaining child authority");
        }
    }
    else
    if (!poster.terminate_and_settle(diagnostic)) {
        fail(diagnostic.c_str());
    }

    std::unique_ptr<Writer> successor;
    const std::string_view retry = test.retry;
    std::shared_ptr<Probe_reader> late_reader;
    if (native_case) {
        require(notification->replay(2).state == Native_notification::Replay_state::REJECTED &&
            !notification->exited(), "wrong occurrence published a native death edge");
        if (std::string_view(test.name) == "native_late_enrollment") {
            late_reader = std::make_shared<Probe_reader>(
                directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>(), 1,
                sintra::detail::ring_directory_policy::caller_directory, notification);
            const auto committed = late_reader->start_reading(1);
            require(committed.end - committed.begin == 1 && *committed.begin == k_payload,
                "late enrollment lost the intact committed payload");
            const auto edge = late_reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
            require(edge.begin == edge.end && !late_reader->is_stopping(),
                "late enrolled reader did not consume its retained native exit edge");
        }
        if (std::string_view(test.name) == "native_transient_failure") {
            sintra::detail::test_hooks::s_ipc_binary_post_error = EIO;
            const auto failed = notification->replay(1);
            require(failed.state == Native_notification::Replay_state::PENDING && failed.error &&
                notification->pending(), "failed native replay discarded its checked obligation");
        }
        if (std::string_view(test.name) == "native_successor_rejection") {
            auto successor = std::make_unique<Writer>(
                directory, k_ring_name, sintra::test::pick_ring_elements<uint32_t>());
            const auto head = reader->get_leading_sequence();
            require(notification->replay(1).state == Native_notification::Replay_state::REJECTED &&
                reader->get_leading_sequence() == head,
                "predecessor native proof changed successor ownership or committed head");
            s_reader_released = true;
            waiter.join();
            require(valid && completed, "successor admission lost the predecessor's committed payload");
            successor->write_commit(k_payload + 1);
            require(reader->get_leading_sequence() == head + 1,
                "rejected predecessor replay damaged the live successor");
            const auto next = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
            require(next.end - next.begin == 1 && *next.begin == k_payload + 1,
                "live successor payload was damaged by rejected predecessor replay");
            reader->request_stop();
            reader->done_reading();
            return 0;
        }
        if (recovery_enabled) {
            const std::string_view name = test.name;
            if (name == "native_completion_new_enrollment") {
                sintra::detail::test_hooks::s_ipc_wakeup_operation = [](const char* stage, const void* object) {
                    if (std::string_view(stage) == "ring_native_posting_released") {
                        s_completion_arrived = true;
                        while (!s_completion_continue.load()) { std::this_thread::yield(); }
                    }
                    if (tl_new_enrollment && std::string_view(stage) == "ring_native_enrollment_before_slot_lock") {
                        s_new_enrollment_arrived = true;
                    }
                    wakeup_hook(stage, object);
                };
                Native_notification::Replay_state old_result = Native_notification::Replay_state::REJECTED;
                std::thread recovery([&] { old_result = notification->replay(1).state; });
                if (!await([] { return s_completion_arrived.load(); })) {
                    fixture_failure("old replay did not release posting before completion");
                }
                std::atomic<bool> enrolled{false};
                std::thread admission([&] {
                    tl_new_enrollment = true;
                    late_reader = std::make_shared<Probe_reader>(directory, k_ring_name,
                        sintra::test::pick_ring_elements<uint32_t>(), 1,
                        sintra::detail::ring_directory_policy::caller_directory, notification);
                    enrolled = true;
                });
                if (!await([] { return s_new_enrollment_arrived.load(); })) {
                    fixture_failure("genuinely new reader did not reach slot admission during old completion");
                }
                require(!enrolled && notification->pending(), "slot exclusion did not retain late admission duty");
                s_completion_continue = true;
                recovery.join();
                admission.join();
                sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
                require(old_result == Native_notification::Replay_state::COMPLETE && enrolled && notification->pending(),
                    "old replay completion erased the genuinely new reader's later enrollment obligation");
                const auto data = late_reader->start_reading(1);
                require(data.end - data.begin == 1 && *data.begin == k_payload,
                    "new enrollment across completion lost intact committed data");
            }
            if (name == "native_signal_handler_callback" || name == "native_exception_handler_callback" ||
                name == "native_held_logger_handler") {
                s_exception_notification = static_cast<Probe_notification*>(notification.get());
                sintra::set_log_callback([](sintra::log_level, const char* message, void*) {
                    ++s_native_log_callbacks;
                    const bool held = s_exception_notification->gate_holder() ==
                        sintra::detail::current_process_instance();
                    std::fprintf(stderr, "%s", message);
                    std::fprintf(stderr, "original debug handler application callback: inspection_held=%d callbacks=%u\n",
                        int(held), s_native_log_callbacks.load());
                    std::fflush(stderr);
                    if (held) { std::_Exit(78); }
                });
                sintra::Log_stream(sintra::log_level::info) << "ordinary logging outside native inspection\n";
                require(s_native_log_callbacks == 1 && s_exception_notification->gate_holder() == 0,
                    "ordinary callback behavior outside inspection changed");
                s_native_log_callbacks = 0;
                if (name == "native_held_logger_handler") {
                    s_expect_held_logger = true;
                    std::thread holder([] {
                        std::unique_lock<std::mutex> lock(sintra::detail::log_mutex());
                        s_held_logger = true;
                        for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
                    });
                    holder.detach();
                    require(await([] { return s_held_logger.load(); }), "ordinary logger mutex was not held before interruption");
                }
                sintra::detail::set_debug_pause_active(true);
                sintra::detail::test_hooks::s_debug_pause_entered = [](const char* reason) noexcept {
                    sintra::detail::native_diagnostic("installed handler terminal observer, callbacks=",
                        s_native_log_callbacks.load(), "\n");
                    (void)reason;
                    std::_Exit(s_native_log_callbacks == 0 && (!s_expect_held_logger || s_held_logger) ? 0 : 78);
                };
                const bool exception = name == "native_exception_handler_callback";
                if (!exception) {
                    require(std::signal(SIGABRT, sintra::detail::debug_signal_handler) != SIG_ERR,
                        "could not install original debug signal handler");
                }
#if defined(_WIN32)
                if (exception) {
                    require(AddVectoredExceptionHandler(1, sintra::detail::debug_vectored_exception_handler) != nullptr,
                        "could not install original debug exception handler");
                }
#endif
                sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void*) {
                    if (std::string_view(stage) == "mutex_inspection_gate_acquired") {
                        if (s_exception_stage) {
#if defined(_WIN32)
                            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
                        } else {
                            std::raise(SIGABRT);
                        }
                    }
                };
                s_exception_stage = exception ? "installed_exception_handler" : nullptr;
                (void)notification->replay(1);
                fail("original installed debug handler did not reach its pause boundary");
            }
            const bool throwing_replay = name == "native_flush_throw_cleanup" ||
                name == "native_backend_throw_cleanup" || name == "native_release_throw_cleanup";
            if (throwing_replay) {
                s_exception_notification = static_cast<Probe_notification*>(notification.get());
                s_exception_stage = name == "native_flush_throw_cleanup" ? "ring_flush_before_post" :
                    name == "native_backend_throw_cleanup" ? "semaphore_post_failed" : "before_zero_store";
                std::set_terminate(exception_termination);
                if (name == "native_release_throw_cleanup") {
                    sintra::detail::test_hooks::s_spinlock_event = [](const void*,
                        sintra::detail::test_hooks::spinlock_event event) {
                        if (event == sintra::detail::test_hooks::spinlock_event::before_zero_store) {
                            throw std::runtime_error("fixture spinlock release exception");
                        }
                    };
                }
                else {
                    if (name == "native_backend_throw_cleanup") {
                        sintra::detail::test_hooks::s_ipc_binary_post_error = EIO;
                    }
                    sintra::detail::test_hooks::s_ipc_wakeup_operation = [](const char* stage, const void* object) {
                        if (std::string_view(stage) == s_exception_stage) {
                            throw std::runtime_error("fixture checked posting exception");
                        }
                        wakeup_hook(stage, object);
                    };
                }
                const auto failed = notification->replay(1);
                sintra::detail::test_hooks::s_spinlock_event = nullptr;
                sintra::detail::test_hooks::s_ipc_wakeup_operation = wakeup_hook;
                sintra::detail::test_hooks::s_ipc_binary_post_error = 0;
                // The wake lets the live reader acquire this same posting lock.
                // Check release after its cleanup, not during its legitimate
                // concurrent ownership of the shared word.
                if (name == "native_release_throw_cleanup" || name == "native_flush_throw_cleanup") {
                    require(await([&] { return completed.load(); }),
                        "throwing replay did not deliver the native wake to its parked reader");
                }
                require(failed.state == Native_notification::Replay_state::PENDING && failed.error &&
                    notification->pending() && s_exception_notification->gate_holder() == 0 &&
                    s_exception_notification->posting_owner() == 0,
                    "throwing replay lost its checked duty or retained a live lock/gate");
                require(sintra::detail::test_hooks::take_observation_failure() != nullptr,
                    "checked replay swallowed the deferred instrumentation failure");
            }
            if (name == "native_live_stall_callback") {
                s_exception_notification = static_cast<Probe_notification*>(notification.get());
                std::atomic<bool> held{false};
                std::thread holder([&] {
                    sintra::spinlock::locker lock(s_exception_notification->reader_lock());
                    held = true;
                    for (;;) { std::this_thread::yield(); }
                });
                holder.detach();
                require(await([&] { return held.load(); }), "diagnostic fixture did not hold its reader lock");
                sintra::detail::set_debug_pause_active(false);
                sintra::test::prepare_for_intentional_crash();
#if defined(_WIN32)
                SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
                require(_set_error_mode(_OUT_TO_STDERR) != -1,
                    "diagnostic fixture could not select noninteractive CRT errors");
#endif
                require(std::signal(SIGABRT, diagnostic_abort) != SIG_ERR,
                    "diagnostic fixture could not observe its intentional abort");
                sintra::detail::test_hooks::s_spinlock_cpu = [](sintra::detail::spinlock_cpu_sample& sample) {
                    sample.ns = s_diagnostic_cpu.fetch_add(3'000'000'000ull);
                    return true;
                };
                sintra::set_log_callback([](sintra::log_level, const char*, void*) {
                    ++s_native_log_callbacks;
                    std::fprintf(stderr, "application callback entered while native inspection gate was held=%d\n",
                        int(s_exception_notification->gate_holder() == sintra::detail::current_process_instance()));
                    std::fflush(stderr);
                });
                (void)notification->replay(1);
                fail("diagnostic fixture did not reach its live-holder abort policy");
            }
            std::atomic<unsigned> application_callbacks{0};
            sintra::set_log_callback([](sintra::log_level, const char*, void* counter) {
                ++*static_cast<std::atomic<unsigned>*>(counter);
            }, &application_callbacks);
            const auto result = notification->replay(1);
            sintra::set_log_callback(nullptr);
            if (std::string_view(test.action) == "publication" &&
                result.state == Native_notification::Replay_state::REJECTED)
            {
                fail("partial mutex publication rejected exact native replay");
            }
            require(application_callbacks == 0,
                "native replay invoked application code inside its ownership inspection gate");
            require(result.state == Native_notification::Replay_state::COMPLETE &&
                !notification->pending(), "armed native owner did not complete notification replay");
        }
    }
    else
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
    if (!native_case) {
        reader->done_reading();
    }
    if (native_case) {
        // Consumed death is not a permanently true empty-return predicate.
        s_reader_arrived = false;
        s_reader_released = true;
        s_reader_stage = "ring_wait_prepared";
        s_wait_thread = 0;
        s_wait_address = 0;
        completed = false;
        std::thread next_wait([&] {
            tl_reader = true;
            const auto empty = reader->wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
            valid = empty.begin == empty.end && reader->is_stopping();
            completed = true;
        });
        require(await(native_reader_is_parked) && s_reader_arrived && !completed,
            "consumed native death edge returned repeatedly instead of parking");
        std::printf("%s: native wait confirmed after consumed exit edge\n", test.name);
        std::fflush(stdout);
        reader->request_stop();
        next_wait.join();
        require(valid && completed, "stop after native recovery did not finish the parked reader");
        reader->done_reading();
        if (late_reader) {
            late_reader->request_stop();
            late_reader->done_reading();
        }
        require(poster.observe_exit_retained() == Exact_child_state::exited,
            "native child authority was consumed before replay and reader cleanup completed");
    }
    return 0;
}

bool supervise_case(const std::string& binary, const wakeup_case_t& test, bool recovery_enabled = true)
{
    sintra::test::Temp_ring_dir directory(test.name);
    const std::string path = directory.str();
    Exact_child scenario(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), recovery_enabled ? "--scenario" : "--scenario-without-recovery",
        test.name, path.c_str(), nullptr};
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
        if (argc == 3 && std::string_view(argv[1]) == "--exhausted-reader-owner") {
            return run_exhausted_reader_owner(argv[2]);
        }
#if !defined(_WIN32)
        if (argc == 2 && std::string_view(argv[1]) == "--post-fork-callback") { return run_post_fork_callback(); }
#endif
        if (argc == 4 && std::string_view(argv[1]) == "--poster") {
            return run_poster(find_case(argv[2]), argv[3]);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--scenario") {
            return run_scenario(binary, find_case(argv[2]), argv[3]);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--scenario-without-recovery") {
            return run_scenario(binary, find_case(argv[2]), argv[3], false);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--recoverer") {
            return run_recoverer(find_case(argv[2]), argv[3]);
        }
        if (argc == 5 && std::string_view(argv[1]) == "--recoverer" &&
            std::string_view(argv[4]) == "second")
        {
            return run_recoverer(find_case(argv[2]), argv[3], true);
        }
        if (argc == 3 && std::string_view(argv[1]) == "--case") {
            return supervise_case(binary, find_case(argv[2])) ? 0 : 1;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--case-without-recovery") {
            return supervise_case(binary, find_case(argv[2]), false) ? 0 : 1;
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
