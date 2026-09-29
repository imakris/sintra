#include <sintra/sintra.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
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
    DWORD64 module_end = 0;
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
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(module) + dos->e_lfanew);
    return {nt_wait, wait_ex, lock, create, module,
        reinterpret_cast<DWORD64>(module) + nt->OptionalHeader.SizeOfImage};
}

Native_wait_path native_wait_path()
{
    const auto create_address = reinterpret_cast<DWORD64>(&pthread_create);
    const auto lock_address = reinterpret_cast<DWORD64>(&pthread_mutex_lock);
    HMODULE linked_module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(create_address), &linked_module))
    {
        const auto create = winpthreads_function_range(linked_module, create_address);
        const auto lock = winpthreads_function_range(linked_module, lock_address);
        if (create.begin && lock.begin) {
            return native_wait_path(linked_module, create, lock);
        }
    }
    // Auto-imported function addresses can name thunks without unwind records.
    // Exports give the DLL's code addresses without decoding those thunks.
    const HMODULE module = GetModuleHandleA("libwinpthread-1.dll");
    return native_wait_path(module,
        native_function_range(module, "pthread_create"),
        native_function_range(module, "pthread_mutex_lock"));
}

// In the source-audited winpthreads creation path, pop_pthread_mem locks
// mtx_pthr_locked, which pthread-key cleanup holds. Require its private caller
// between the lock and create frames in one suspended stack.
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
                const auto address = last_stack[i + 1];
                const auto base = SymGetModuleBase64(process, address);
                const auto* unwind = static_cast<const RUNTIME_FUNCTION*>(
                    SymFunctionTableAccess64(process, address));
                if (base == reinterpret_cast<DWORD64>(path.winpthreads) &&
                    address < path.module_end && unwind &&
                    base + unwind->BeginAddress <= address &&
                    address < base + unwind->EndAddress &&
                    base + unwind->EndAddress <= path.module_end &&
                    base + unwind->BeginAddress != path.lock.begin &&
                    base + unwind->BeginAddress != path.create.begin)
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

constexpr const char* k_inspector_flag = "--native-stack-inspector";

int inspect_native_stack(char* argv[])
{
    const auto number = [&](unsigned index) {
        return std::strtoull(argv[index], nullptr, 10);
    };
    const DWORD process_id = static_cast<DWORD>(number(2));
    const DWORD admitting_thread_id = static_cast<DWORD>(number(3));
    const HANDLE start = reinterpret_cast<HANDLE>(number(4));
    const HANDLE suspended = reinterpret_cast<HANDLE>(number(5));
    const bool force_timeout = number(6) != 0;
    Native_wait_path path{{number(7), number(8)}, {number(9), number(10)},
        {number(11), number(12)}, {number(13), number(14)},
        reinterpret_cast<HMODULE>(number(15)), number(16)};
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
        THREAD_QUERY_INFORMATION, FALSE, admitting_thread_id);
    const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, process_id);
    if (!thread || !process || !SymInitialize(process, nullptr, TRUE) ||
        WaitForSingleObject(start, 10000) != WAIT_OBJECT_0)
    {
        return 2;
    }
    if (force_timeout) {
        if (SuspendThread(thread) == DWORD(-1)) {
            return 3;
        }
        SetEvent(suspended);
        Sleep(INFINITE);
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
            admitting_thread_id, DWORD(0), last_count);
        for (unsigned i = 0; i < last_count; ++i) {
            std::fprintf(stderr, "frame %u: %llx\n", i,
                static_cast<unsigned long long>(last_stack[i]));
        }
    }
    CloseHandle(process);
    return observed ? 0 : 1;
}

bool run_native_inspector(DWORD admitting_thread_id, const Native_wait_path& path,
    bool force_timeout)
{
    // Only this helper suspends the target. The parent retains the target handle
    // and can repair one outstanding suspension after any helper exit.
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, admitting_thread_id);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const HANDLE start = CreateEventA(&security, TRUE, FALSE, nullptr);
    const HANDLE suspended = CreateEventA(&security, TRUE, FALSE, nullptr);
    char executable[32768]{};
    const DWORD length = GetModuleFileNameA(nullptr, executable, sizeof(executable));
    bool started = false;
    bool timed_out = false;
    bool confirmed_suspended = false;
    DWORD exit_code = 1;
    DWORD previous_suspend_count = DWORD(-1);
    if (thread && start && suspended && length && length < sizeof(executable)) {
        std::string command = std::string("\"") + executable + "\" " + k_inspector_flag;
        const std::array<DWORD64, 15> values{{GetCurrentProcessId(), admitting_thread_id,
            reinterpret_cast<DWORD64>(start), reinterpret_cast<DWORD64>(suspended),
            force_timeout ? 1u : 0u, path.nt_wait.begin, path.nt_wait.end,
            path.wait_ex.begin, path.wait_ex.end, path.lock.begin, path.lock.end,
            path.create.begin, path.create.end,
            reinterpret_cast<DWORD64>(path.winpthreads), path.module_end}};
        for (const auto value : values) {
            command += " " + std::to_string(value);
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION helper{};
        started = CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &helper) != 0;
        if (started) {
            const HANDLE waits[]{suspended, helper.hProcess};
            // From start until ResumeThread, make only native calls: the target
            // may be suspended while it owns any parent-process runtime lock.
            SetEvent(start);
            if (force_timeout) {
                confirmed_suspended = WaitForMultipleObjects(2, waits, FALSE, 10000) ==
                    WAIT_OBJECT_0;
            }
            const DWORD result = WaitForSingleObject(helper.hProcess,
                force_timeout && confirmed_suspended ? 50 : 15000);
            timed_out = result == WAIT_TIMEOUT;
            if (result != WAIT_OBJECT_0) {
                TerminateProcess(helper.hProcess, 124);
            }
            WaitForSingleObject(helper.hProcess, INFINITE);
            GetExitCodeProcess(helper.hProcess, &exit_code);
            previous_suspend_count = ResumeThread(thread);
            CloseHandle(helper.hThread);
            CloseHandle(helper.hProcess);
        }
    }
    if (thread) {
        CloseHandle(thread);
    }
    if (start) {
        CloseHandle(start);
    }
    if (suspended) {
        CloseHandle(suspended);
    }
    if (force_timeout) {
        const bool repaired = started && confirmed_suspended && timed_out &&
            exit_code == 124 && previous_suspend_count == 1;
        if (repaired) {
            std::fprintf(stderr, "NATIVE_INSPECTOR_TIMEOUT_REPAIRED: admitting=%lu\n",
                admitting_thread_id);
        }
        return check(repaired, "timed-out inspector is reaped before target resumes");
    }
    return check(started && !timed_out && exit_code == 0 && previous_suspend_count == 0,
        "external native stack inspection completes");
}

bool native_creation_waits_on_cleanup(DWORD admitting_thread_id,
    DWORD cleanup_thread_id)
{
    const auto path = native_wait_path();
    if (!check(path.nt_wait.begin != 0, "native function ranges initialized")) {
        return false;
    }
    const bool repaired = run_native_inspector(admitting_thread_id, path, true);
    const bool observed = repaired && run_native_inspector(admitting_thread_id, path, false);
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
#if defined(__MINGW32__)
    if (argc == 17 && std::strcmp(argv[1], k_inspector_flag) == 0) {
        return inspect_native_stack(argv);
    }
#endif
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
