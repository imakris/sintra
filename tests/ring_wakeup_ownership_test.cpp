#include "isolated_test_support.h"
#include "ring_fixture_test_support.h"

#include <sintra/rings.h>

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using word = uint32_t;
constexpr const char* k_ring_name = "wakeup_ownership";
constexpr word k_payload = 0x51a7;
constexpr auto k_deadline = std::chrono::seconds(8);

struct Probe_writer : sintra::Ring_W<word>
{
    using sintra::Ring_W<word>::Ring_W;
    using sintra::Ring_W<word>::m_control;

    bool no_pending_wakeups()
    {
        sintra::spinlock::locker lock(m_control->m_spinlock);
        if (m_control->sleeping_stack.size() != 0) {
            return false;
        }
        for (auto& semaphore : m_control->dirty_semaphores) {
            if (semaphore.wait_for(std::chrono::nanoseconds(0)) !=
                sintra::sintra_ring_semaphore::wait_result::timeout)
            {
                return false;
            }
        }
        return true;
    }
};

struct Park_gate
{
    std::mutex mutex;
    std::condition_variable changed;
    std::array<bool, sintra::max_process_index> indices{};
    size_t prepared = 0;
    bool duplicate = false;
    bool released = false;

    void prepare(int index)
    {
        std::unique_lock lock(mutex);
        if (index < 0 || index >= sintra::max_process_index || indices[index]) {
            duplicate = true;
        }
        else {
            indices[index] = true;
            ++prepared;
        }
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }

    bool wait(size_t count)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, k_deadline, [&] { return prepared == count; }) &&
            !duplicate;
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};

Park_gate* s_park_gate = nullptr;

void pause_before_wait(int index)
{
    s_park_gate->prepare(index);
}

struct Parked_readers
{
    Park_gate gate;
    std::vector<std::unique_ptr<sintra::Ring_R<word>>> readers;
    std::vector<std::thread> threads;
    std::atomic<size_t> returned{0};
    std::atomic<size_t> received{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stop_requested{false};

    void start(const std::string& directory, size_t capacity, size_t count)
    {
        s_park_gate = &gate;
        sintra::detail::test_hooks::s_ring_wait_prepared = &pause_before_wait;
        for (size_t i = 0; i != count; ++i) {
            readers.push_back(std::make_unique<sintra::Ring_R<word>>(
                directory, k_ring_name, capacity));
        }
        for (auto& reader : readers) {
            threads.emplace_back([&, reader = reader.get()] {
                try {
                    reader->start_reading();
                    while (!stop_requested.load()) {
                        const auto range = reader->wait_for_new_data();
                        if (range.begin && range.end != range.begin) {
                            if (range.end - range.begin == 1 && *range.begin == k_payload) {
                                ++received;
                            }
                            else {
                                failed = true;
                            }
                            break;
                        }
                    }
                    reader->done_reading();
                    ++returned;
                }
                catch (...) {
                    failed = true;
                }
            });
        }
    }

    void stop()
    {
        stop_requested = true;
        for (auto& reader : readers) {
            reader->request_stop();
        }
        gate.release();
    }

    void join()
    {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    ~Parked_readers()
    {
        stop();
        join();
        sintra::detail::test_hooks::s_ring_wait_prepared = nullptr;
        s_park_gate = nullptr;
    }
};

bool stop_preserves_capacity()
{
    sintra::test::Temp_ring_dir directory("wakeup_stop");
    const size_t capacity = sintra::test::pick_ring_elements<word>();
    Probe_writer writer(directory.str(), k_ring_name, capacity);
    {
        Parked_readers stopped;
        stopped.start(directory.str(), capacity, 1);
        if (!stopped.gate.wait(1)) {
            std::fprintf(stderr, "Initial reader did not prepare its wait\n");
            return false;
        }
        // Stop exactly after registration, before the semaphore wait starts.
        stopped.stop();
        stopped.join();
        if (stopped.failed || stopped.returned != 1) {
            return false;
        }
    }
    writer.unblock_global();
    if (!writer.no_pending_wakeups()) {
        std::fprintf(stderr, "Stopped reader left a wakeup for its successor\n");
        return false;
    }

    Parked_readers full;
    full.start(directory.str(), capacity, sintra::max_process_index);
    const bool all_prepared = full.gate.wait(sintra::max_process_index);
    full.stop();
    full.join();
    if (!all_prepared || full.failed || full.returned != sintra::max_process_index) {
        std::fprintf(stderr, "Stopped reader reduced wakeup capacity: prepared=%zu expected=%d\n",
            full.gate.prepared, int(sintra::max_process_index));
        return false;
    }
    return writer.no_pending_wakeups();
}

bool dead_reader_capacity_is_recovered(const std::string& binary)
{
    sintra::test::Temp_ring_dir directory("wakeup_dead_child");
    const size_t capacity = sintra::test::pick_ring_elements<word>();
    Probe_writer writer(directory.str(), k_ring_name, capacity);
    sintra::test::Exact_child child(std::chrono::seconds(5));
    const std::string path = directory.str();
    const char* args[] = {binary.c_str(), "--wakeup-child", path.c_str(), nullptr};
    if (!child.spawn(binary.c_str(), args)) {
        std::fprintf(stderr, "Cannot launch owned child: %s\n", child.error().c_str());
        return false;
    }

    bool parked = false;
    const auto deadline = std::chrono::steady_clock::now() + k_deadline;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            sintra::spinlock::locker lock(writer.m_control->m_spinlock);
            parked = writer.m_control->sleeping_stack.size() == sintra::max_process_index;
        }
        if (parked || child.poll() != sintra::test::Exact_child_state::running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::string diagnostic;
    const bool terminated = child.terminate_and_settle(diagnostic);
    if (!parked || !terminated) {
        std::fprintf(stderr, "Owned child did not park/settle: parked=%d %s\n",
            parked, diagnostic.c_str());
        return false;
    }

    // Ordered flushing used to discard all indices whose reader had died.
    writer.unblock_global();
    sintra::test::checked_scavenge_orphans(*writer.m_control);
    if (!writer.no_pending_wakeups()) {
        std::fprintf(stderr, "Dead reader left a wakeup for its successor\n");
        return false;
    }
    Parked_readers recovered;
    recovered.start(directory.str(), capacity, sintra::max_process_index);
    const bool prepared = recovered.gate.wait(sintra::max_process_index);
    if (!prepared) {
        recovered.stop();
        recovered.join();
        std::fprintf(stderr, "Dead readers exhausted the surviving ring's wakeups\n");
        return false;
    }
    writer.write_commit(k_payload);
    recovered.gate.release();
    recovered.join();
    return !recovered.failed && recovered.returned == sintra::max_process_index &&
        recovered.received == sintra::max_process_index && writer.no_pending_wakeups();
}

int run_child(const char* directory)
{
    Parked_readers readers;
    readers.start(directory, sintra::test::pick_ring_elements<word>(), sintra::max_process_index);
    if (!readers.gate.wait(sintra::max_process_index)) {
        return 2;
    }
    // The parent owns the exact process and terminates it at the parked seam.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace

namespace ownership_cleanup {

namespace fs = std::filesystem;
using sintra::test::isolated::await;
using sintra::test::isolated::require;
using sintra::test::isolated::signal_file;
using sintra::test::isolated::fixture_failure;
using sintra::test::isolated::case_t;
using sintra::test::Exact_child;
using sintra::test::Exact_child_state;
using sintra::test::ring_fixture::Probe_reader;
using sintra::test::ring_fixture::Probe_writer;
using sintra::test::ring_fixture::k_ring_name;
using sintra::test::ring_fixture::k_payload;
using sintra::test::ring_fixture::tl_reader;
using sintra::test::ring_fixture::s_ownership_mutex;
using sintra::test::ring_fixture::wakeup_hook;
using sintra::test::ring_fixture::reader_prepared;
using sintra::test::ring_fixture::native_reader_is_parked;
using sintra::test::ring_fixture::s_reset_report_writer;
using sintra::test::ring_fixture::s_reset_reports;
using sintra::test::ring_fixture::s_reset_report_with_gate;
using sintra::test::ring_fixture::observe_reset_report;

constexpr case_t k_cases[] = {
    {"exhausted_admission_reset_failure", "", "exhausted_admission"},
#if !defined(_WIN32)
    {"native_multithreaded_fork_exec", "", "fork_exec"},
#endif
    {"writer_constructor_before_cas_cleanup", "mutex_before_owner_cas", "custody"},
    {"writer_constructor_after_cas_cleanup", "mutex_after_owner_cas", "custody"},
    {"lifecycle_before_cas_cleanup", "mutex_before_owner_cas", "custody"},
    {"lifecycle_after_cas_cleanup", "mutex_after_owner_cas", "custody"},
    {"reader_admission_reset_failure", "", "custody"},
    {"writer_close_multiple_slot_errors", "", "close_errors"},
    {"writer_close_backend_retry", "", "retirement"},
    {"reader_stop_backend_retry", "", "retirement"},
    {"reader_release_throw_cleanup", "", "retirement"},
    {"reader_retirement_reset_retry", "", "retirement"},
    {"reader_retirement_persistent_reset", "", "retirement"},
    {"writer_close_throw_cleanup", "", "retirement"},
    {"writer_close_flush_throw_cleanup", "", "retirement"},
    {"writer_close_post_throw_cleanup", "", "retirement"},
#if !defined(_WIN32)
    {"writer_close_count_throw_cleanup", "", "retirement"},
#endif
};

const char* s_throw_mutex_stage = nullptr;
std::atomic<bool> s_probe_thrown{false};

int s_exhausted_first_slot = -1;

int run_exhausted_reader_owner(const char* directory)
{
    const fs::path path(directory);
    {
        std::ofstream capture(path / "reader_owner_instance");
        capture << sintra::detail::current_process_instance() << '\n';
        require(bool(capture), "cannot publish exact reader owner's instance");
    }
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
    // published so the next ordinary admission must scavenge them.
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
    Exact_child child(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), "--exhausted-reader-owner", directory, nullptr};
    require(child.spawn(binary.c_str(), args), "cannot spawn reader owner: " + child.error());
    require(await([&] { return fs::exists(path / "reader_owner_full"); }), "reader owner did not exhaust the actual slots");
    uint64_t instance = 0;
    std::ifstream(path / "reader_owner_instance") >> instance;
    require(sintra::detail::process_instance_pid(instance) == uint32_t(child.pid()),
        "reader owner instance does not match the spawned child");
    int registered = -1;
    std::ifstream(path / "reader_owner_slot") >> registered;
    require(registered >= 0 && writer.free_slots() == 0 && writer.pending_count_unlocked() == 1,
        "exhausted fixture did not publish every actual admission and registration");
    // Keep the Windows named token pinned in the surviving process too.
    writer.post_token(registered);
    signal_file(path / "reader_owner_hold");
    require(await([&] { return child.poll() == Exact_child_state::exited; }) &&
        fs::exists(path / "reader_owner_locked") && writer.posting_owner() == instance,
        "reader owner did not die while holding its published posting lock");
    const auto guards = writer.guard_counts();
    require(guards != 0, "dead actual snapshots did not retain their guard counts");
    s_exhausted_reset_fault = true;
    sintra::detail::test_hooks::s_ipc_wakeup_operation = [](const char* stage, const void*) {
        observe_reset_report(stage);
        if (std::string_view(stage) == "ring_reset_before_backend") {
            ++s_exhausted_reset_attempts;
            if (s_exhausted_reset_fault) { sintra::detail::test_hooks::s_ipc_reset_error = EIO; }
        }
    };
    bool rejected = false;
    try {
        Probe_reader reader(directory, k_ring_name, elements);
    }
    catch (const sintra::ring_acquisition_failure_exception&) { rejected = true; }
    require(rejected && s_exhausted_reset_attempts == unsigned(sintra::max_process_index) &&
        writer.free_slots() == 0 && writer.guard_counts() == guards && writer.pending_count_unlocked() == 1,
        "failed checked scavenging reset retired or reused a guard/token-bearing slot");
    require(s_reset_reports == unsigned(sintra::max_process_index) && !s_reset_report_with_gate,
        "reader scavenging reported a reset error while a shared gate was held");
    require(writer.consume_token(registered), "failed checked reset erased the original native token");
    writer.post_token(registered);
    s_exhausted_reset_fault = false;
    {
        Probe_reader reader(directory, k_ring_name, elements);
        require(writer.free_slots() == sintra::max_process_index - 1 && writer.guard_counts() == 0 &&
            writer.pending_count_unlocked() == 0 && !writer.consume_token(registered),
            "successful checked drain did not settle dead registrations before ACTIVE reuse");
        reader.start_reading();
        require(writer.guard_counts() != 0, "reused reader slot could not acquire an actual snapshot");
        reader.done_reading();
    }
    sintra::detail::test_hooks::s_ipc_wakeup_operation = nullptr;
    require(writer.free_slots() == sintra::max_process_index && child.exited_with_code(0),
        "reused admission leaked a slot or reader owner did not exit normally");
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

struct fork_probe_t
{
    const void* mutex;
    int channel;
    pid_t child = -1;
    char acknowledgement = 0;
};
fork_probe_t* s_fork_probe = nullptr;

int run_fork_exec_case(const std::string& binary, const char* directory)
{
    const auto elements = sintra::test::pick_ring_elements<uint32_t>();
    Probe_reader reader(directory, k_ring_name, elements);
    auto& mutex = reader.ownership_mutex();
    const auto instance = sintra::detail::current_process_instance();
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
    fork_probe_t probe{&mutex, channel[0]};
    s_fork_probe = &probe;
    sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void* object) {
        if (object == s_fork_probe->mutex && std::string_view(stage) == "mutex_after_owner_cas") {
            s_fork_probe->child = ::fork();
            if (s_fork_probe->child > 0) {
                ssize_t count;
                do {
                    count = ::read(s_fork_probe->channel, &s_fork_probe->acknowledgement, 1);
                } while (count < 0 && errno == EINTR);
                if (count != 1) { s_fork_probe->acknowledgement = 0; }
            }
        }
    };
    bool acquired = false;
    {
        sintra::spinlock::locker readers(reader.reader_lock());
        acquired = mutex.test_try_lock_throwing();
        // Only PID-aware native guard release runs in the child on this path.
        // No Ring destruction or application callback is attempted before exec.
    }
    if (probe.child == 0) {
        const bool parent_custody = acquired && reader.inspection_holder() == instance &&
            reader.reader_slot_owner() == instance;
        sintra::detail::native_diagnostic("fork child native output; parent shared custody preserved=", parent_custody, "\n");
        const char ready = parent_custody ? 1 : 0;
        ssize_t written;
        do { written = ::write(channel[1], &ready, 1); } while (written < 0 && errno == EINTR);
        if (!parent_custody || written != 1) { ::_exit(81); }
        ::execv(program, args);
        ::_exit(82);
    }
    sintra::detail::test_hooks::s_mutex_operation = nullptr;
    s_fork_probe = nullptr;
    release = true;
    logger.join();
    ::close(channel[0]);
    ::close(channel[1]);
    require(probe.child > 0 && probe.acknowledgement == 1 && acquired &&
        reader.inspection_holder() == 0 && reader.reader_slot_owner() == 0,
        "fork-child guard abandonment modified the parent's actual shared custody");
    mutex.unlock();
    int status = 0;
    require(await([&] { return ::waitpid(probe.child, &status, WNOHANG) == probe.child; }) &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork/exec child did not restore ordinary callback behavior");
    Probe_writer writer(directory, k_ring_name, elements);
    writer.write_commit(k_payload);
    require(writer.get_leading_sequence() == 1, "parent writer lost ownership after child-image abandonment");
    return 0;
}
#endif

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

int run_retirement_case(const case_t& test, const char* directory)
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

int run_custody_case(const case_t& test, const char* directory)
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

int run_case(const std::string& binary, const case_t& test, const char* directory)
{
    const std::string_view action = test.action;
    if (action == "exhausted_admission") { return run_exhausted_admission_case(binary, directory); }
#if !defined(_WIN32)
    if (action == "fork_exec") { return run_fork_exec_case(binary, directory); }
#endif
    if (action == "close_errors") { return run_close_error_case(directory); }
    if (action == "retirement") { return run_retirement_case(test, directory); }
    if (action == "custody") { return run_custody_case(test, directory); }
    throw std::runtime_error("unknown wakeup ownership case");
}

} // namespace ownership_cleanup

int main(int argc, char* argv[])
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--exhausted-reader-owner") {
            return ownership_cleanup::run_exhausted_reader_owner(argv[2]);
        }
#if !defined(_WIN32)
        if (argc == 2 && std::string_view(argv[1]) == "--post-fork-callback") {
            return ownership_cleanup::run_post_fork_callback();
        }
#endif
        if (const auto result = sintra::test::isolated::run_cases(
            argc, argv, ownership_cleanup::k_cases, ownership_cleanup::run_case))
        {
            return *result;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--wakeup-child") {
            return run_child(argv[2]);
        }
        const bool death_only = argc > 1 && std::string_view(argv[1]) == "--death-only";
        const bool stop_only = argc > 1 && std::string_view(argv[1]) == "--stop-only";
        bool ok = true;
        if (!death_only) {
            ok = stop_preserves_capacity();
        }
        if (!stop_only) {
            ok = dead_reader_capacity_is_recovered(
                sintra::test::get_binary_path(argc, argv)) && ok;
        }
        return ok ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "ring_wakeup_ownership_test: %s\n", error.what());
        return 1;
    }
}
