#include <sintra/detail/ipc/rings.h>

#include "exact_child_test_support.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winternl.h>
#else
#include <csignal>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/user.h>
#endif
#endif

namespace {

using sintra::detail::interprocess_semaphore;
using sintra::detail::ips_backend;
constexpr auto k_deadline = std::chrono::seconds(8);
constexpr uint32_t k_terminated = 71;
std::atomic<unsigned> s_entries{0};
std::atomic<uint64_t> s_thread{0};
std::atomic<uintptr_t> s_address{0};
void* s_mapping = nullptr;
size_t s_mapping_size = 0;
bool s_invalidated = false;
bool s_invalidate_on_wait = false;
interprocess_semaphore* s_post_at_entry = nullptr;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "untimed semaphore check failed: %s\n", message);
        std::fflush(stderr);
        std::_Exit(2);
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

// Shared placement exercises the same native keying as interprocess users.
template <typename T>
class Shared_object
{
public:
    Shared_object()
    {
#if defined(_WIN32)
        m_size = sizeof(T);
        m_memory = VirtualAlloc(nullptr, m_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        require(m_memory != nullptr, "VirtualAlloc failed");
#else
        m_size = size_t(::sysconf(_SC_PAGESIZE));
        require(m_size >= sizeof(T), "invalid page size");
        m_memory = ::mmap(nullptr, m_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        require(m_memory != MAP_FAILED, "mmap failed");
#endif
        m_object = new (m_memory) T();
        if constexpr (std::is_same_v<T, ips_backend>) {
            m_object->init_default(0);
        }
    }

    ~Shared_object()
    {
#if !defined(_WIN32)
        if (s_invalidated && m_memory == s_mapping) {
            return;
        }
#endif
        if constexpr (std::is_same_v<T, ips_backend>) {
            m_object->destroy();
        }
        m_object->~T();
#if defined(_WIN32)
        require(VirtualFree(m_memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
#else
        require(::munmap(m_memory, m_size) == 0, "munmap failed");
#endif
    }

    T& get() { return *m_object; }
    void arm_native_failure()
    {
        s_mapping = m_memory;
        s_mapping_size = m_size;
        s_invalidate_on_wait = true;
    }

private:
    void* m_memory = nullptr;
    T* m_object = nullptr;
    size_t m_size = 0;
};

void wait_hook(const char* stage, const void* object)
{
    if (std::string_view(stage) != "semaphore_before_wait") {
        return;
    }
    ++s_entries;
#if defined(_WIN32)
    s_thread = GetCurrentThreadId();
#elif defined(__linux__)
    s_thread = uint64_t(::syscall(SYS_gettid));
#elif defined(__APPLE__)
    s_thread = uint64_t(pthread_mach_thread_np(pthread_self()));
#elif defined(__FreeBSD__)
    long id = 0;
    require(::thr_self(&id) == 0, "thr_self failed");
    s_thread = uint64_t(id);
#endif
    s_address = reinterpret_cast<uintptr_t>(object);
    if (s_post_at_entry) {
        auto* semaphore = std::exchange(s_post_at_entry, nullptr);
        semaphore->post();
    }
    if (s_invalidate_on_wait && !s_invalidated) {
        // Only this isolated child loses its fixture resource before native entry.
#if defined(_WIN32)
        auto& backend = *const_cast<ips_backend*>(static_cast<const ips_backend*>(object));
        const HANDLE handle = sintra::detail::ips_win_ensure_handle(backend);
        require(handle && CloseHandle(handle), "cannot invalidate owned wait handle");
#else
        require(::munmap(s_mapping, s_mapping_size) == 0, "cannot invalidate owned wait page");
#endif
        s_invalidated = true;
    }
}

bool native_parked()
{
    const auto id = s_thread.load();
    if (!id || !s_address.load()) {
        return false;
    }
#if defined(_WIN32)
    using Query = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    require(query != nullptr, "NtQuerySystemInformation unavailable");
    std::vector<unsigned char> buffer(256 * 1024);
    ULONG needed = 0;
    auto status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &needed);
    if (status < 0 && needed > buffer.size()) {
        buffer.resize(needed);
        status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &needed);
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
            for (ULONG i = 0; i < process->NumberOfThreads; ++i) {
                if (reinterpret_cast<uintptr_t>(threads[i].ClientId.UniqueThread) == id) {
                    return threads[i].ThreadState == 5;
                }
            }
            return false;
        }
        if (!process->NextEntryOffset || process->NextEntryOffset > buffer.size() - offset) {
            return false;
        }
        offset += process->NextEntryOffset;
    }
    return false;
#elif defined(__linux__)
    std::ifstream stream("/proc/self/task/" + std::to_string(id) + "/syscall");
    long number = -1;
    uintptr_t address = 0, operation = 0, expected = 0, timeout = 1;
    stream >> number >> std::hex >> address >> operation >> expected >> timeout;
    return bool(stream) && number == SYS_futex && address == s_address.load() &&
        operation == FUTEX_WAIT && expected == 0 && timeout == 0;
#elif defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    return thread_info(mach_port_t(id), THREAD_BASIC_INFO,
               reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS &&
        info.run_state == TH_STATE_WAITING;
#elif defined(__FreeBSD__)
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD, int(getpid())};
    size_t size = 0;
    if (::sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) {
        return false;
    }
    std::vector<kinfo_proc> entries(size / sizeof(kinfo_proc) + 4);
    size = entries.size() * sizeof(kinfo_proc);
    if (::sysctl(mib, 4, entries.data(), &size, nullptr, 0) != 0) {
        return false;
    }
    for (size_t i = 0; i < size / sizeof(kinfo_proc); ++i) {
        if (uint64_t(entries[i].ki_tid) == id && entries[i].ki_stat == SSLEEP &&
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

void test_token_and_park()
{
    interprocess_semaphore ready(1);
    ready.wait();
    require(!ready.try_wait(), "initial token was not acquired exactly once");
#if !defined(_WIN32)
    require(s_entries == 0, "initial POSIX token must avoid native entry");
#endif
    s_entries = 0;
    s_thread = 0;
    s_address = 0;

    Shared_object<interprocess_semaphore> shared;
    auto& semaphore = shared.get();
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        semaphore.wait();
        done = true;
    });
    require(await(native_parked), "waiter did not park in the native untimed wait");
    const auto before = s_entries.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(!done && native_parked(), "idle wait returned without a token");
    require(s_entries <= before + 1, "normal idle wait repeatedly entered native backend");
    std::fprintf(stderr, "native untimed park: entries=%u address=%llx thread=%llu\n",
        s_entries.load(), (unsigned long long)s_address.load(), (unsigned long long)s_thread.load());
#if !defined(_WIN32)
    const auto wake_entries = s_entries.load();
    require(sintra::detail::posix_wake_some(reinterpret_cast<uint32_t*>(s_address.load()), 1),
        "native wake without token failed");
    require(await([&] { return s_entries > wake_entries && native_parked(); }),
        "native wake did not recheck and park again");
    require(!done, "native wake was incorrectly reported as token acquisition");
#endif
    semaphore.post();
    require(await([&] { return done.load(); }), "posted token did not complete acquisition");
    waiter.join();
    require(!semaphore.try_wait(), "one post was consumed more than once");
}

#if !defined(_WIN32)
void test_mismatch_recheck()
{
    Shared_object<interprocess_semaphore> shared;
    s_post_at_entry = &shared.get();
    shared.get().wait();
    require(s_entries == 1 && !shared.get().try_wait(),
        "native mismatch did not consume exactly the token published before entry");
}

std::atomic<uint32_t>* s_signal_count = nullptr;
volatile sig_atomic_t s_signal_seen = 0;

void interrupted_wait(int)
{
    // Lock-free signal publication makes acquisition authoritative after EINTR.
    s_signal_count->store(1, std::memory_order_release);
    s_signal_seen = 1;
}

void test_interruption_recheck()
{
    Shared_object<ips_backend> shared;
    auto& backend = shared.get();
    s_signal_count = &sintra::detail::P(backend).count;
    struct sigaction action{};
    action.sa_handler = interrupted_wait;
    require(sigemptyset(&action.sa_mask) == 0 && ::sigaction(SIGUSR1, &action, nullptr) == 0,
        "cannot install interrupt handler");
    std::atomic<bool> done{false};
    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        acquired = backend.wait();
        done = true;
    });
    require(await(native_parked), "interrupt target did not park");
    require(::pthread_kill(waiter.native_handle(), SIGUSR1) == 0, "pthread_kill failed");
    require(await([&] { return done.load(); }), "interrupted acquisition did not observe signal token");
    waiter.join();
    require(s_signal_seen && acquired && s_entries == 1 && !backend.try_wait(),
        "interruption did not recheck the authoritative token before another native entry");
}
#endif

int native_error()
{
#if defined(_WIN32)
    return EINVAL;
#else
    return EFAULT;
#endif
}

void generic_terminated()
{
    const bool expected = s_invalidated && errno == native_error() && !std::current_exception();
    std::fprintf(stderr, "generic wait termination: errno=%d expected=%d\n", errno, int(expected));
    std::fflush(stderr);
    std::_Exit(expected ? k_terminated : 72);
}

void test_failure(std::string_view which)
{
    if (which == "backend_error") {
        Shared_object<ips_backend> shared;
        shared.arm_native_failure();
        errno = 0;
        const bool acquired = shared.get().wait();
        const int error = errno;
        require(s_invalidated && !acquired && error == native_error() && s_entries == 1,
            "backend must retain native errno and reject acquisition without retrying");
    }
    else
    if (which == "checked_error") {
        Shared_object<sintra::sintra_ring_semaphore> shared;
        shared.arm_native_failure();
        bool caught = false;
        try {
            shared.get().wait();
        }
        catch (const std::system_error& error) {
            caught = error.code() == std::error_code(native_error(), std::generic_category());
        }
        require(s_invalidated && caught && s_entries == 1,
            "checked wait must preserve native failure through its exception channel");
    }
    else {
        Shared_object<interprocess_semaphore> shared;
        shared.arm_native_failure();
        std::set_terminate(generic_terminated);
        shared.get().wait();
        require(false, "generic wait returned normally without acquisition");
    }
    // Resource invalidation is confined to this child; no later fixture use is legal.
    std::_Exit(0);
}

int run_child(std::string_view which)
{
    sintra::detail::test_hooks::s_ipc_wakeup_operation = wait_hook;
    if (which == "token_and_park") {
        test_token_and_park();
    }
#if !defined(_WIN32)
    else
    if (which == "mismatch_recheck") {
        test_mismatch_recheck();
    }
    else
    if (which == "interruption_recheck") {
        test_interruption_recheck();
    }
#endif
    else {
        test_failure(which);
    }
    return 0;
}

bool supervise(const char* program, const char* which, uint32_t expected)
{
    sintra::test::Exact_child child(std::chrono::seconds(2));
    const char* arguments[] = {program, "--child", which, nullptr};
    bool passed = child.spawn(program, arguments);
    if (passed) {
        passed = await([&] { return child.poll() != sintra::test::Exact_child_state::running; });
    }
    if (passed) {
        passed = child.exited_with_code(expected);
    }
    std::string settlement;
    const bool settled = child.terminate_and_settle(settlement);
    std::fprintf(stderr, "%s: %s; expected=%u; settled=%d %s\n",
        which, child.describe_status().c_str(), expected, int(settled), settlement.c_str());
    return passed && settled;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
        return run_child(argv[2]);
    }
    bool passed = supervise(argv[0], "token_and_park", 0);
#if !defined(_WIN32)
    passed = supervise(argv[0], "mismatch_recheck", 0) && passed;
    passed = supervise(argv[0], "interruption_recheck", 0) && passed;
#endif
    passed = supervise(argv[0], "backend_error", 0) && passed;
    passed = supervise(argv[0], "checked_error", 0) && passed;
    passed = supervise(argv[0], "generic_error", k_terminated) && passed;
    return passed ? 0 : 1;
}
