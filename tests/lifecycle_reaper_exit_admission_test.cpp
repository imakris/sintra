#include <sintra/sintra.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__MINGW32__)
#include <wct.h>
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
    unsigned before_construct = 0;
    unsigned body_completed = 0;
    unsigned post_join = 0;
    bool release_cleanup = false;
    bool admission_failed = false;
#if defined(__MINGW32__)
    DWORD cleanup_thread_id = 0;
    std::atomic<bool> native_construct_entered{false};
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
        if (std::strcmp(stage, "before_construct") == 0) {
            ++phase->before_construct;
        }
        else if (std::strcmp(stage, "body_completed") == 0) {
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
// Winpthreads holds its creation mutex across pthread-key cleanup. Windows
// reports this wait only as a blocked thread, without naming that mutex. The
// marker runs after the callback and just before std::thread construction;
// together they establish that the wait is inside native creation.
bool native_creation_waits_on_cleanup(DWORD admitting_thread_id,
    DWORD cleanup_thread_id, const std::atomic<bool>& native_construct_entered)
{
    const HWCT session = OpenThreadWaitChainSession(0, nullptr);
    if (!check(session != nullptr, "wait-chain session opened")) {
        return false;
    }
    bool observed = false;
    std::array<WAITCHAIN_NODE_INFO, 16> last_nodes{};
    DWORD last_count = 0;
    DWORD last_error = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    do {
        std::array<WAITCHAIN_NODE_INFO, 16> nodes{};
        DWORD count = static_cast<DWORD>(nodes.size());
        BOOL cycle = FALSE;
        if (native_construct_entered.load(std::memory_order_acquire)) {
            if (GetThreadWaitChain(session, 0, 0, admitting_thread_id,
                    &count, nodes.data(), &cycle))
            {
                last_nodes = nodes;
                last_count = count;
                observed = count >= 1 && nodes[0].ObjectType == WctThreadType &&
                    nodes[0].ThreadObject.ThreadId == admitting_thread_id &&
                    nodes[0].ObjectStatus == WctStatusBlocked;
            }
            else {
                last_error = GetLastError();
            }
        }
        if (!observed) {
            std::this_thread::yield();
        }
    } while (!observed && std::chrono::steady_clock::now() < deadline);
    CloseThreadWaitChainSession(session);
    if (!observed) {
        std::fprintf(stderr, "WCT admitting=%lu cleanup=%lu entered=%d count=%lu error=%lu\n",
            admitting_thread_id, cleanup_thread_id,
            native_construct_entered.load(std::memory_order_acquire),
            last_count, last_error);
        for (DWORD i = 0; i < last_count && i < last_nodes.size(); ++i) {
            std::fprintf(stderr, "WCT node %lu type=%d status=%d thread=%lu\n",
                i, last_nodes[i].ObjectType, last_nodes[i].ObjectStatus,
                last_nodes[i].ObjectType == WctThreadType ?
                    last_nodes[i].ThreadObject.ThreadId : 0);
        }
    }
    if (observed) {
        std::fprintf(stderr,
            "NATIVE_WAIT: admitting=%lu blocked inside std::thread while cleanup=%lu holds winpthreads lock\n",
            admitting_thread_id, cleanup_thread_id);
    }
    return check(cleanup_thread_id != 0 && observed,
        "native winpthreads creation waits during cleanup");
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
            std::unique_lock<std::mutex> lock(admission_mutex);
            admission_changed.wait(lock, [&] { return release_admission; });
        }
        ordinary_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
        sintra::s_mproc->start_owned_lifecycle_worker([] {});
        ordinary_returned.store(true, std::memory_order_release);
    });
    std::atomic<bool> key_set{false};
    std::thread application([&] {
        key_set.store(set_key(key, phase), std::memory_order_release);
    });
    bool valid = check(phase.wait([&] { return phase.cleanup_entered == 1; }),
        "application pthread-key destructor entered");
    sintra::detail::test_hooks::s_owned_lifecycle_native_construct_marker.store(
        &phase.native_construct_entered, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(admission_mutex);
        release_admission = true;
    }
    admission_changed.notify_all();
    const bool reached = phase.wait([&] { return phase.before_construct >= 1; });
    valid &= check(reached,
        "first ordinary admission reached native construction");
    if (reached) {
        valid &= native_creation_waits_on_cleanup(
            ordinary_thread_id.load(std::memory_order_acquire), phase.cleanup_thread_id,
            phase.native_construct_entered);
    }
    sintra::detail::test_hooks::s_owned_lifecycle_native_construct_marker.store(
        nullptr, std::memory_order_release);
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
            std::unique_lock<std::mutex> lock(admission_mutex);
            admission_changed.wait(lock, [&] { return release_admission; });
        }
        other_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
        sintra::s_mproc->start_owned_lifecycle_worker([] {});
        other_returned.store(true, std::memory_order_release);
    });
    std::atomic<bool> key_set{false};
    sintra::s_mproc->start_owned_lifecycle_worker([&] {
        key_set.store(set_key(key, phase), std::memory_order_release);
    });
    bool valid = check(phase.wait([&] {
        return phase.cleanup_entered == 1 && phase.body_completed >= 1;
    }), "owned pthread-key destructor overlaps body completion");
    sintra::detail::test_hooks::s_owned_lifecycle_native_construct_marker.store(
        &phase.native_construct_entered, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(admission_mutex);
        release_admission = true;
    }
    admission_changed.notify_all();
    const bool reached = phase.wait([&] { return phase.before_construct >= 2; });
    valid &= check(reached,
        "separate admission reached native construction");
    if (reached) {
        valid &= native_creation_waits_on_cleanup(
            other_thread_id.load(std::memory_order_acquire), phase.cleanup_thread_id,
            phase.native_construct_entered);
    }
    sintra::detail::test_hooks::s_owned_lifecycle_native_construct_marker.store(
        nullptr, std::memory_order_release);
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
