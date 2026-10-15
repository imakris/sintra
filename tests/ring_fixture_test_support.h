#pragma once

#include "isolated_test_support.h"

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

namespace sintra::test::ring_fixture {

namespace fs = std::filesystem;
using Reader = sintra::Ring_R<uint32_t>;
using Writer = sintra::Ring_W<uint32_t>;
using isolated::require;
using isolated::signal_file;
constexpr const char* k_ring_name = "crash_wakeup";
constexpr uint32_t k_payload = 0x51a7u;

inline fs::path s_directory;
inline std::string s_poster_stage;
inline std::string s_reader_stage;
inline std::atomic<bool> s_reader_arrived{false};
inline std::atomic<bool> s_reader_released{false};
inline std::atomic<uint64_t> s_wait_thread{0};
inline std::atomic<uintptr_t> s_wait_address{0};
inline std::atomic<int> s_reader_index{-1};
inline thread_local bool tl_reader = false;
inline const void* s_ownership_mutex = nullptr;

inline void wakeup_hook(const char* stage, const void* object)
{
    if (std::string_view(stage) == "ring_writer_before_ownership") {
        s_ownership_mutex = object;
        return;
    }
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

inline void reader_prepared(int index)
{
    s_reader_index = index;
    wakeup_hook("ring_wait_prepared", nullptr);
}

// A pre-syscall hook alone does not prove kernel parking. For the POSIX
// counter-before-wake case, inspect the reader's native state while no token,
// signal or competing poster can release it.
inline bool native_reader_is_parked()
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

    uint64_t posting_owner() const { return m_control->m_spinlock.test_owner(); }
    uint64_t reader_slot_owner() const { return m_control->rs_stack_spinlock.test_owner(); }
    uint64_t inspection_holder() const { return m_control->ownership_mutex.test_gate_holder(); }
    uint64_t writer_owner() const { return m_control->ownership_mutex.test_owner_token(); }
    int pending_count_unlocked() const { return m_control->sleeping_stack.size(); }
    sintra::spinlock& posting_lock() { return m_control->m_spinlock; }
    sintra::spinlock& reader_lock() { return m_control->rs_stack_spinlock; }
    sintra::detail::interprocess_mutex& ownership_mutex() { return m_control->ownership_mutex; }
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

inline Probe_writer* s_reset_report_writer = nullptr;
inline std::atomic<unsigned> s_reset_reports{0};
inline std::atomic<bool> s_reset_report_with_gate{false};

inline void observe_reset_report(const char* stage)
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

} // namespace sintra::test::ring_fixture
