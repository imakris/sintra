#include <sintra/sintra.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(__MINGW32__)
#include <dbghelp.h>
#endif

#if !defined(_WIN32) || defined(__MINGW32__)
#include <pthread.h>
#endif

namespace {

using namespace std::chrono_literals;

struct Phase
{
    std::mutex mutex;
    std::condition_variable changed;
    unsigned cleanup_entered = 0;
    unsigned cleanup_done = 0;
    unsigned body_completed = 0;
    unsigned post_join = 0;
    bool release_cleanup = false;
    bool admission_failed = false;
#if defined(__MINGW32__)
    DWORD cleanup_thread_id = 0;
#endif

    template <typename Predicate>
    bool wait(Predicate predicate)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 10s, predicate);
    }

    void admit_from_cleanup() noexcept
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++cleanup_entered;
#if defined(__MINGW32__)
            cleanup_thread_id = GetCurrentThreadId();
#endif
            changed.notify_all();
            changed.wait(lock, [this] { return release_cleanup; });
        }
        try {
            sintra::s_mproc->start_owned_lifecycle_worker([] {});
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            admission_failed = true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++cleanup_done;
        }
        changed.notify_all();
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_cleanup = true;
        }
        changed.notify_all();
    }
};

std::atomic<Phase*> g_phase{nullptr};

void on_worker_event(const char* stage, uint64_t) noexcept
{
    auto* phase = g_phase.load(std::memory_order_acquire);
    if (!phase) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(phase->mutex);
        if (std::strcmp(stage, "body_completed") == 0) {
            ++phase->body_completed;
        }
        else if (std::strcmp(stage, "post_join") == 0) {
            ++phase->post_join;
        }
    }
    phase->changed.notify_all();
}

bool check(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return value;
}

#if !defined(_WIN32) || defined(__MINGW32__)
void key_cleanup(void* context)
{
    static_cast<Phase*>(context)->admit_from_cleanup();
}

bool set_key(pthread_key_t key, Phase& phase)
{
    return pthread_setspecific(key, &phase) == 0;
}
#endif

#if defined(__MINGW32__)
struct Native_function_range
{
    DWORD64 begin = 0;
    DWORD64 end = 0;

    bool contains(DWORD64 address) const
    {
        return begin <= address && address < end;
    }
};

Native_function_range native_function_range(HMODULE module, const char* name)
{
    const FARPROC entry = module ? GetProcAddress(module, name) : nullptr;
    DWORD64 image_base = 0;
    const auto* unwind = entry ? RtlLookupFunctionEntry(
        reinterpret_cast<DWORD64>(entry), &image_base, nullptr) : nullptr;
    return unwind ? Native_function_range{
        image_base + unwind->BeginAddress, image_base + unwind->EndAddress} :
        Native_function_range{};
}

struct Native_wait_path
{
    Native_function_range nt_wait;
    Native_function_range wait_ex;
    Native_function_range lock;
    Native_function_range create;
    HMODULE winpthreads = nullptr;
};

Native_function_range winpthreads_function_range(HMODULE module, DWORD64 address)
{
    DWORD64 image_base = 0;
    const auto* unwind = RtlLookupFunctionEntry(address, &image_base, nullptr);
    return unwind && image_base == reinterpret_cast<DWORD64>(module) ?
        Native_function_range{image_base + unwind->BeginAddress,
            image_base + unwind->EndAddress} : Native_function_range{};
}

Native_wait_path native_wait_path(HMODULE module,
    const Native_function_range& create, const Native_function_range& lock)
{
    const auto nt_wait = native_function_range(GetModuleHandleA("ntdll.dll"),
        "NtWaitForSingleObject");
    const auto wait_ex = native_function_range(GetModuleHandleA("KernelBase.dll"),
        "WaitForSingleObjectEx");
    if (!nt_wait.begin || !wait_ex.begin || !lock.begin || !create.begin) {
        return {};
    }
    return {nt_wait, wait_ex, lock, create, module};
}

// In the source-audited winpthreads creation path, pop_pthread_mem locks
// mtx_pthr_locked, which pthread-key cleanup holds. Require its private caller
// between the exported lock and create frames in one suspended stack.
bool blocked_in_native_creation_lock(HANDLE thread, HANDLE process,
    const Native_wait_path& path,
    std::array<DWORD64, 32>& last_stack, unsigned& last_count)
{
    if (SuspendThread(thread) == DWORD(-1)) {
        return false;
    }
    bool matched = false;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        last_count = 0;
        while (last_count < last_stack.size() &&
            StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                nullptr))
        {
            const auto address = frame.AddrPC.Offset;
            if (!address) {
                break;
            }
            last_stack[last_count++] = address;
        }
        for (unsigned i = 2; i + 2 < last_count; ++i) {
            if (path.nt_wait.contains(last_stack[i - 2]) &&
                path.wait_ex.contains(last_stack[i - 1]) &&
                path.lock.contains(last_stack[i]) &&
                path.create.contains(last_stack[i + 2]))
            {
                const auto caller = winpthreads_function_range(
                    path.winpthreads, last_stack[i + 1]);
                if (caller.begin && caller.begin != path.lock.begin &&
                    caller.begin != path.create.begin)
                {
                    matched = true;
                    break;
                }
            }
        }
    }
    ResumeThread(thread);
    return matched;
}

bool native_creation_waits_on_cleanup(DWORD admitting_thread_id,
    DWORD cleanup_thread_id)
{
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
        THREAD_QUERY_INFORMATION, FALSE, admitting_thread_id);
    const HANDLE process = GetCurrentProcess();
    const HMODULE winpthreads = GetModuleHandleA("libwinpthread-1.dll");
    const auto create = native_function_range(winpthreads, "pthread_create");
    const auto lock = native_function_range(winpthreads, "pthread_mutex_lock");
    const auto path = native_wait_path(winpthreads, create, lock);
    const bool ready = thread && path.nt_wait.begin &&
        SymInitialize(process, nullptr, TRUE);
    if (!check(ready, "native stack inspection initialized")) {
        if (thread) {
            CloseHandle(thread);
        }
        return false;
    }
    bool observed = false;
    std::array<DWORD64, 32> last_stack{};
    unsigned last_count = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    do {
        observed = blocked_in_native_creation_lock(thread, process, path,
            last_stack, last_count);
        if (!observed) {
            std::this_thread::yield();
        }
    } while (!observed && std::chrono::steady_clock::now() < deadline);
    SymCleanup(process);
    CloseHandle(thread);
    if (!observed) {
        std::fprintf(stderr, "native stack admitting=%lu cleanup=%lu count=%u\n",
            admitting_thread_id, cleanup_thread_id, last_count);
        for (unsigned i = 0; i < last_count; ++i) {
            std::fprintf(stderr, "frame %u: %llx\n", i,
                static_cast<unsigned long long>(last_stack[i]));
        }
    }
    if (observed) {
        std::fprintf(stderr,
            "NATIVE_LOCK_WAIT: admitting=%lu NtWaitForSingleObject -> "
            "WaitForSingleObjectEx -> pthread_mutex_lock -> "
            "private winpthreads caller -> pthread_create; cleanup=%lu\n",
            admitting_thread_id, cleanup_thread_id);
    }
    return check(cleanup_thread_id != 0 && observed,
        "pthread_create waits in pthread_mutex_lock during cleanup");
}

bool first_admission_overlaps_application_cleanup(Phase& phase)
{
    g_phase.store(&phase, std::memory_order_release);
    pthread_key_t key{};
    if (!check(pthread_key_create(&key, &key_cleanup) == 0,
            "application cleanup key created"))
    {
        return false;
    }
    std::mutex admission_mutex;
    std::condition_variable admission_changed;
    bool release_admission = false;
    std::atomic<bool> ordinary_returned{false};
    std::atomic<DWORD> ordinary_thread_id{0};
    std::thread ordinary([&] {
        {
            std::lock_guard<std::mutex> lock(phase.mutex);
            ordinary_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
        }
        phase.changed.notify_all();
        {
            std::unique_lock<std::mutex> lock(admission_mutex);
            admission_changed.wait(lock, [&] { return release_admission; });
        }
        sintra::s_mproc->start_owned_lifecycle_worker([] {});
        ordinary_returned.store(true, std::memory_order_release);
    });
    bool valid = check(phase.wait([&] {
        return ordinary_thread_id.load(std::memory_order_acquire) != 0;
    }), "ordinary admission thread identified before cleanup");
    std::atomic<bool> key_set{false};
    std::thread application([&] {
        key_set.store(set_key(key, phase), std::memory_order_release);
    });
    valid &= check(phase.wait([&] { return phase.cleanup_entered == 1; }),
        "application pthread-key destructor entered");
    {
        std::lock_guard<std::mutex> lock(admission_mutex);
        release_admission = true;
    }
    admission_changed.notify_all();
    if (ordinary_thread_id.load(std::memory_order_acquire) != 0) {
        valid &= native_creation_waits_on_cleanup(
            ordinary_thread_id.load(std::memory_order_acquire), phase.cleanup_thread_id);
    }
    valid &= check(!ordinary_returned.load(std::memory_order_acquire),
        "native construction remains blocked during key cleanup");
    phase.release();
    ordinary.join();
    application.join();
    valid &= check(phase.wait([&] {
        return phase.cleanup_done == 1 && phase.post_join >= 2;
    }), "first ordinary and cleanup admissions complete and join");
    valid &= check(key_set.load() && !phase.admission_failed,
        "application cleanup admits its worker");
    pthread_key_delete(key);
    g_phase.store(nullptr, std::memory_order_release);
    return valid;
}

bool owned_cleanup_overlaps_construction(Phase& phase)
{
    g_phase.store(&phase, std::memory_order_release);
    pthread_key_t key{};
    if (!check(pthread_key_create(&key, &key_cleanup) == 0,
            "owned cleanup key created"))
    {
        return false;
    }
    std::mutex admission_mutex;
    std::condition_variable admission_changed;
    bool release_admission = false;
    std::atomic<bool> other_returned{false};
    std::atomic<DWORD> other_thread_id{0};
    std::thread other([&] {
        {
            std::lock_guard<std::mutex> lock(phase.mutex);
            other_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
        }
        phase.changed.notify_all();
        {
            std::unique_lock<std::mutex> lock(admission_mutex);
            admission_changed.wait(lock, [&] { return release_admission; });
        }
        sintra::s_mproc->start_owned_lifecycle_worker([] {});
        other_returned.store(true, std::memory_order_release);
    });
    bool valid = check(phase.wait([&] {
        return other_thread_id.load(std::memory_order_acquire) != 0;
    }), "separate admission thread identified before owned worker");
    std::atomic<bool> key_set{false};
    sintra::s_mproc->start_owned_lifecycle_worker([&] {
        key_set.store(set_key(key, phase), std::memory_order_release);
    });
    valid &= check(phase.wait([&] {
        return phase.cleanup_entered == 1 && phase.body_completed >= 1;
    }), "owned pthread-key destructor overlaps body completion");
    {
        std::lock_guard<std::mutex> lock(admission_mutex);
        release_admission = true;
    }
    admission_changed.notify_all();
    if (other_thread_id.load(std::memory_order_acquire) != 0) {
        valid &= native_creation_waits_on_cleanup(
            other_thread_id.load(std::memory_order_acquire), phase.cleanup_thread_id);
    }
    valid &= check(!other_returned.load(std::memory_order_acquire),
        "native construction remains blocked during owned-worker cleanup");
    phase.release();
    other.join();
    valid &= check(phase.wait([&] {
        return phase.cleanup_done == 1 && phase.post_join >= 3;
    }), "owned destructor admission and all workers join");
    valid &= check(key_set.load() && !phase.admission_failed,
        "owned key destructor admits its worker");
    pthread_key_delete(key);
    g_phase.store(nullptr, std::memory_order_release);
    return valid;
}
#elif defined(_WIN32)
struct Namespace_tls_admission
{
    Phase* phase = nullptr;
    ~Namespace_tls_admission()
    {
        if (phase) {
            phase->admit_from_cleanup();
        }
    }
};

thread_local Namespace_tls_admission g_namespace_tls_admission;

bool namespace_tls_cleanup_admits(Phase& phase)
{
    g_phase.store(&phase, std::memory_order_release);
    sintra::s_mproc->start_owned_lifecycle_worker([&] {
        g_namespace_tls_admission.phase = &phase;
    });
    bool valid = check(phase.wait([&] {
        return phase.cleanup_entered == 1 && phase.body_completed >= 1;
    }), "namespace-scope TLS destructor entered after body completion");
    phase.release();
    valid &= check(phase.wait([&] {
        return phase.cleanup_done == 1 && phase.post_join >= 2;
    }), "TLS destructor admission returns and both workers join");
    valid &= check(!phase.admission_failed, "TLS destructor admits its worker");
    g_phase.store(nullptr, std::memory_order_release);
    return valid;
}
#else
bool concurrent_posix_key_cleanups_admit(Phase& phase)
{
    g_phase.store(&phase, std::memory_order_release);
    pthread_key_t key{};
    if (!check(pthread_key_create(&key, &key_cleanup) == 0,
            "concurrent POSIX cleanup key created"))
    {
        return false;
    }
    std::atomic<unsigned> keys_set{0};
    for (unsigned i = 0; i < 2; ++i) {
        sintra::s_mproc->start_owned_lifecycle_worker([&] {
            if (set_key(key, phase)) {
                ++keys_set;
            }
        });
    }
    bool valid = check(phase.wait([&] {
        return phase.cleanup_entered == 2 && phase.body_completed >= 2;
    }), "both POSIX key destructors entered after body completion");
    phase.release();
    valid &= check(phase.wait([&] {
        return phase.cleanup_done == 2 && phase.post_join >= 4;
    }), "both cleanup admissions and all joins finish");
    valid &= check(keys_set.load() == 2 && !phase.admission_failed,
        "both POSIX key destructors admit workers");
    pthread_key_delete(key);
    g_phase.store(nullptr, std::memory_order_release);
    return valid;
}
#endif

} // namespace

int main(int argc, char* argv[])
{
    // The Python test runner is the external process watchdog: it terminates
    // a hung case after --timeout 30 and reports the missing acknowledgement.
    sintra::init(argc, argv);
    sintra::detail::test_hooks::s_owned_lifecycle_worker_event.store(
        &on_worker_event, std::memory_order_release);
    bool valid = true;
#if defined(__MINGW32__)
    Phase application_phase;
    Phase owned_phase;
    valid = first_admission_overlaps_application_cleanup(application_phase);
    if (valid) {
        valid = owned_cleanup_overlaps_construction(owned_phase);
    }
#elif defined(_WIN32)
    Phase phase;
    valid = namespace_tls_cleanup_admits(phase);
#else
    Phase phase;
    valid = concurrent_posix_key_cleanups_admit(phase);
#endif
    sintra::s_mproc->join_owned_lifecycle_workers();
    sintra::s_mproc->join_owned_lifecycle_workers();
    sintra::detail::test_hooks::s_owned_lifecycle_worker_event.store(
        nullptr, std::memory_order_release);
    g_phase.store(nullptr, std::memory_order_release);
    valid &= check(sintra::detail::finalize(), "external drain and finalization complete");
    return valid ? 0 : 1;
}
