#include <sintra/detail/utility.h>
#include <sintra/detail/ipc/spinlocked_containers.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/time_utils.h>

#include "test_utils.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <tlhelp32.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__FreeBSD__)
#include "test_process_identity_fakes.h"
#endif

#if defined(__linux__)
#include <pthread.h>
#endif

namespace {

constexpr std::string_view k_failure_prefix = "utility_test: ";

void test_adaptive_function_basic()
{
    int call_count = 0;
    sintra::Adaptive_function af([&call_count]() {
        ++call_count;
    });

    af();
    sintra::test::require_true(call_count == 1,
        k_failure_prefix,
        "Adaptive_function should call the function");

    af();
    sintra::test::require_true(call_count == 2,
        k_failure_prefix,
        "Adaptive_function should be callable multiple times");
}

void test_adaptive_function_copy_constructor()
{
    int call_count = 0;
    sintra::Adaptive_function af1([&call_count]() {
        ++call_count;
    });

    sintra::Adaptive_function af2(af1);

    af1();
    sintra::test::require_true(call_count == 1,
        k_failure_prefix,
        "Original should work after copy");

    af2();
    sintra::test::require_true(call_count == 2,
        k_failure_prefix,
        "Copy should work");
}

void test_adaptive_function_copy_assignment()
{
    int count1 = 0;
    int count2 = 0;

    sintra::Adaptive_function af1([&count1]() { ++count1; });
    sintra::Adaptive_function af2([&count2]() { ++count2; });

    af2 = af1;

    af1();
    sintra::test::require_true(count1 == 1,
        k_failure_prefix,
        "Original should work after assignment");

    af2();
    sintra::test::require_true(count1 == 2,
        k_failure_prefix,
        "Assigned function should call original's function");
    sintra::test::require_true(count2 == 0,
        k_failure_prefix,
        "Original function of af2 should not be called");
}

void test_cstring_vector_from_lvalue()
{
    std::vector<std::string> strings = {"hello", "world", "test"};
    sintra::C_string_vector csv(strings);

    sintra::test::require_true(csv.size() == 3,
        k_failure_prefix,
        "C_string_vector size should match input");

    const char* const* data = csv.v();
    sintra::test::require_true(data != nullptr,
        k_failure_prefix,
        "C_string_vector data should not be null");
    sintra::test::require_true(std::string(data[0]) == "hello",
        k_failure_prefix,
        "First element should match");
    sintra::test::require_true(std::string(data[1]) == "world",
        k_failure_prefix,
        "Second element should match");
    sintra::test::require_true(std::string(data[2]) == "test",
        k_failure_prefix,
        "Third element should match");
}

void test_cstring_vector_from_rvalue()
{
    std::vector<std::string> strings = {"foo", "bar"};
    sintra::C_string_vector csv(std::move(strings));

    sintra::test::require_true(csv.size() == 2,
        k_failure_prefix,
        "C_string_vector size should match input");

    const char* const* data = csv.v();
    sintra::test::require_true(data != nullptr,
        k_failure_prefix,
        "C_string_vector data should not be null");
    sintra::test::require_true(std::string(data[0]) == "foo",
        k_failure_prefix,
        "First element should match");
    sintra::test::require_true(std::string(data[1]) == "bar",
        k_failure_prefix,
        "Second element should match");
}

void test_cstring_vector_empty()
{
    std::vector<std::string> empty;
    sintra::C_string_vector csv(empty);

    sintra::test::require_true(csv.size() == 0,
        k_failure_prefix,
        "Empty C_string_vector should have size 0");
}

void test_env_key_of()
{
    using sintra::detail::env_key_of;

    sintra::test::require_true(env_key_of("FOO=bar") == "FOO",
        k_failure_prefix,
        "env_key_of should split at '='");
    sintra::test::require_true(env_key_of("NOEQ") == "NOEQ",
        k_failure_prefix,
        "env_key_of should return whole string when no '='");
    sintra::test::require_true(env_key_of("A=B=C") == "A",
        k_failure_prefix,
        "env_key_of should split at first '='");

#ifdef _WIN32
    sintra::test::require_true(env_key_of("=C:=C:\\working") == "=C:",
        k_failure_prefix,
        "env_key_of should preserve a Windows drive pseudo-variable name");
    sintra::test::require_true(env_key_of(std::wstring(L"=D:=D:\\working")) == L"=D:",
        k_failure_prefix,
        "env_key_of should preserve a wide Windows drive pseudo-variable name");
#endif
}

#ifndef _WIN32
void test_build_environment_entries()
{
    using sintra::detail::build_environment_entries;
    using sintra::detail::env_key_of;

    const std::string key            = "SINTRA_TEST_ENV_KEY";
    const std::string override_entry = key + "=OVERRIDE";

    setenv(key.c_str(), "ORIGINAL", 1);

    auto env_before = build_environment_entries({});
    sintra::test::require_true(!env_before.empty(),
        k_failure_prefix,
        "build_environment_entries should return environment entries");

    auto env_after = build_environment_entries({override_entry});

    int  match_count    = 0;
    bool found_override = false;
    for (const auto& entry : env_after) {
        if (env_key_of(entry) == key) {
            ++match_count;
            if (entry == override_entry) {
                found_override = true;
            }
        }
    }

    sintra::test::require_true(match_count == 1,
        k_failure_prefix,
        "override should replace existing entry exactly once");
    sintra::test::require_true(found_override,
        k_failure_prefix,
        "override entry should be present");
}
#else
void test_windows_environment_block()
{
    using sintra::detail::build_environment_block;
    using sintra::detail::env_key_equal;
    using sintra::detail::env_key_of;
    using sintra::detail::merge_env_overrides;

    std::vector<std::wstring> entries {
        L"=C:=C:\\stale-c",
        L"=c:=C:\\duplicate-stale-c",
        L"=D:=D:\\retained-d",
        L"Ordinary=preserved",
        L"Path=C:\\stale-path"
    };
    const std::vector<std::wstring> overrides {
        L"=C:=C:\\changed-c",
        L"=E:=E:\\added-e",
        L"PATH=C:\\changed-path"
    };

    merge_env_overrides(entries, overrides);

    auto count_key = [&](const std::wstring& key) {
        return std::count_if(entries.begin(), entries.end(), [&](const std::wstring& entry) {
            return env_key_equal(env_key_of(entry), key);
        });
    };
    auto contains_entry = [&](const std::wstring& expected) {
        return std::find(entries.begin(), entries.end(), expected) != entries.end();
    };

    sintra::test::require_true(count_key(L"=C:") == 1 &&
            contains_entry(L"=C:=C:\\changed-c"),
        k_failure_prefix,
        "a drive pseudo-variable override should remove stale same-key entries");
    sintra::test::require_true(count_key(L"=D:") == 1 &&
            contains_entry(L"=D:=D:\\retained-d"),
        k_failure_prefix,
        "overriding one drive pseudo-variable should retain other drive entries");
    sintra::test::require_true(count_key(L"=E:") == 1 &&
            contains_entry(L"=E:=E:\\added-e"),
        k_failure_prefix,
        "multiple drive pseudo-variables should coexist after merging overrides");
    sintra::test::require_true(count_key(L"PATH") == 1 &&
            contains_entry(L"PATH=C:\\changed-path"),
        k_failure_prefix,
        "ordinary Windows environment names should remain case-insensitive");
    sintra::test::require_true(
        env_key_equal(L"SINTRA_\u00c4_CASE", L"sintra_\u00e4_case"),
        k_failure_prefix,
        "Windows environment names should use locale-independent Unicode case matching");
    sintra::test::require_true(contains_entry(L"Ordinary=preserved"),
        k_failure_prefix,
        "an unrelated ordinary environment entry should be preserved");

    const auto block = build_environment_block({
        "SINTRA_ZZZ_ENVIRONMENT_ORDER=last",
        "=D:=D:\\ordered-d",
        "SINTRA_AAA_ENVIRONMENT_ORDER=first",
        "=C:=C:\\ordered-c",
        "SINTRA_ENVIRONMENT_PREFIX=short",
        "SINTRA_ENVIRONMENT_PREFIX1=long"
    });

    std::vector<std::wstring> block_entries;
    for (const wchar_t* cursor = block.data(); *cursor != L'\0'; ) {
        block_entries.emplace_back(cursor);
        cursor += block_entries.back().size() + 1;
    }

    auto count_block_key = [&](const std::wstring& key) {
        return std::count_if(
            block_entries.begin(),
            block_entries.end(),
            [&](const std::wstring& entry) {
                return env_key_equal(env_key_of(entry), key);
            });
    };

    sintra::test::require_true(block.size() >= 2 &&
            block[block.size() - 2] == L'\0' && block.back() == L'\0',
        k_failure_prefix,
        "a Windows Unicode environment block should be double-null terminated");
    sintra::test::require_true(count_block_key(L"=C:") == 1 &&
            std::find(block_entries.begin(), block_entries.end(), L"=C:=C:\\ordered-c") !=
                block_entries.end(),
        k_failure_prefix,
        "the final Windows block should contain the changed C drive entry once");
    sintra::test::require_true(count_block_key(L"=D:") == 1 &&
            std::find(block_entries.begin(), block_entries.end(), L"=D:=D:\\ordered-d") !=
                block_entries.end(),
        k_failure_prefix,
        "the final Windows block should contain the D drive entry once");
    bool keys_are_sorted = true;
    for (std::size_t i = 1; i < block_entries.size(); ++i) {
        const auto lhs_key = env_key_of(block_entries[i - 1]);
        const auto rhs_key = env_key_of(block_entries[i]);
        const int comparison = CompareStringOrdinal(
            lhs_key.c_str(),
            -1,
            rhs_key.c_str(),
            -1,
            TRUE);
        if (comparison != CSTR_LESS_THAN && comparison != CSTR_EQUAL) {
            keys_are_sorted = false;
            break;
        }
    }
    sintra::test::require_true(keys_are_sorted,
        k_failure_prefix,
        "a Windows Unicode environment block should be sorted by name");

    auto find_block_key = [&](const std::wstring& key) {
        return std::find_if(
            block_entries.begin(),
            block_entries.end(),
            [&](const std::wstring& entry) {
                return env_key_equal(env_key_of(entry), key);
            });
    };
    const auto prefix_key  = find_block_key(L"SINTRA_ENVIRONMENT_PREFIX");
    const auto prefix1_key = find_block_key(L"SINTRA_ENVIRONMENT_PREFIX1");
    sintra::test::require_true(
        prefix_key != block_entries.end() &&
            prefix1_key != block_entries.end() &&
            prefix_key < prefix1_key,
        k_failure_prefix,
        "a name should sort before a longer name that shares its prefix");
}
#endif

void test_spinlocked_umap_scoped_erase()
{
    // Test scoped_access::erase(iterator) - exercises the uncovered code path
    sintra::detail::spinlocked<std::unordered_map, std::string, int> map;

    // Insert some entries
    map.with_lock([](auto& inner) { inner.emplace("one", 1); });
    map.with_lock([](auto& inner) { inner.emplace("two", 2); });
    map.with_lock([](auto& inner) { inner.emplace("three", 3); });

    // Use scoped access to iterate and erase
    {
        auto scoped = map.scoped();
        for (auto it = scoped.begin(); it != scoped.end(); ) {
            if (it->second == 2) {
                it = scoped.erase(it);  // This is the uncovered line!
            }
            else {
                ++it;
            }
        }
    }

    // Verify the entry was erased
    auto scoped = map.scoped();
    sintra::test::require_true(scoped.get().size() == 2,
        k_failure_prefix,
        "Map should have 2 entries after erase");
    sintra::test::require_true(scoped.get().find("two") == scoped.get().end(),
        k_failure_prefix,
        "'two' should be erased");
    sintra::test::require_true(scoped.get().find("one") != scoped.get().end(),
        k_failure_prefix,
        "'one' should remain");
    sintra::test::require_true(scoped.get().find("three") != scoped.get().end(),
        k_failure_prefix,
        "'three' should remain");
}

// On Windows, cleanup_stale_swarm_directories leaves preserved scratch alone
// unless a test root confines it.
bool stale_directory_cleanup_runs()
{
#if defined(_WIN32)
    const char* preserve_scratch = std::getenv("SINTRA_PRESERVE_SCRATCH");
    const char* test_root        = std::getenv("SINTRA_TEST_ROOT");
    const bool preserve_without_test_root =
        preserve_scratch && preserve_scratch[0] != '\0' && preserve_scratch[0] != '0' &&
        (!test_root || test_root[0] == '\0');
    return !preserve_without_test_root;
#else
    return true;
#endif
}

void test_process_utility_helpers()
{
    const auto current_pid = static_cast<std::uint32_t>(sintra::get_current_pid());

    sintra::test::require_true(!sintra::is_process_alive(0), k_failure_prefix,
        "pid 0 should not be reported alive");
    sintra::test::require_true(sintra::is_process_alive(current_pid), k_failure_prefix,
        "current process should be reported alive");
    sintra::test::require_true(!sintra::query_process_start_stamp(0).has_value(), k_failure_prefix,
        "pid 0 should not have a process start stamp");

#ifndef _WIN32
    const pid_t child_pid = ::fork();
    sintra::test::require_true(child_pid >= 0, k_failure_prefix,
        "fork should succeed for zombie liveness check");

    if (child_pid == 0) {
        ::_exit(0);
    }

    bool exited_child_reported_dead = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (!sintra::is_process_alive(static_cast<std::uint32_t>(child_pid))) {
            exited_child_reported_dead = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    int   child_status = 0;
    pid_t waited       = 0;
    do {
        waited = ::waitpid(child_pid, &child_status, 0);
    } while (waited < 0 && errno == EINTR);

    sintra::test::require_true(waited == child_pid, k_failure_prefix,
        "waitpid should reap the zombie liveness child");
    sintra::test::require_true(exited_child_reported_dead, k_failure_prefix,
        "exited unreaped child should not be reported alive");
#endif

    const auto current_start = sintra::current_process_start_stamp().value_or(0);
    const auto scratch = sintra::test::unique_scratch_directory("utility_process_utils");

    sintra::run_marker_record_t record{};
    record.pid                  = current_pid;
    record.start_stamp          = current_start;
    record.created_monotonic_ns = sintra::monotonic_now_ns();
    record.recovery_occurrence  = 7;

    sintra::test::require_true(sintra::write_run_marker(scratch, record), k_failure_prefix,
        "write_run_marker should write into an existing directory");

    const auto read_record = sintra::read_run_marker(sintra::run_marker_path(scratch));
    sintra::test::require_true(read_record.has_value(), k_failure_prefix,
        "read_run_marker should parse a valid marker");
    sintra::test::require_true(
        read_record->pid == record.pid &&
        read_record->start_stamp == record.start_stamp &&
        read_record->created_monotonic_ns == record.created_monotonic_ns &&
        read_record->recovery_occurrence == record.recovery_occurrence,
        k_failure_prefix,
        "read_run_marker should preserve all marker fields");

    sintra::mark_run_directory_for_cleanup(scratch);
    sintra::test::require_true(
        std::filesystem::exists(sintra::run_marker_cleanup_path(scratch)) &&
        !std::filesystem::exists(sintra::run_marker_path(scratch)),
        k_failure_prefix,
        "mark_run_directory_for_cleanup should move the marker to cleanup state");

    sintra::remove_run_marker_files(scratch);
    sintra::test::require_true(
        !std::filesystem::exists(sintra::run_marker_path(scratch)) &&
        !std::filesystem::exists(sintra::run_marker_cleanup_path(scratch)),
        k_failure_prefix,
        "remove_run_marker_files should remove marker and cleanup files");

    std::ofstream bad_marker(sintra::run_marker_path(scratch), std::ios::trunc);
    bad_marker << "pid=not-a-pid\n";
    bad_marker.close();
    sintra::test::require_true(!sintra::read_run_marker(sintra::run_marker_path(scratch)).has_value(),
        k_failure_prefix,
        "read_run_marker should reject malformed numeric fields");

    const auto cleanup_base = sintra::test::unique_scratch_directory("utility_process_cleanup") / "private";
    sintra::test::require_true(sintra::detail::create_private_directory(cleanup_base),
        k_failure_prefix, "create private cleanup root");
    const auto stale_dir    = cleanup_base / "stale";
    sintra::test::require_true(sintra::detail::create_private_directory(stale_dir),
        k_failure_prefix, "create private stale directory");
    sintra::test::require_true(sintra::detail::write_private_file(
        sintra::run_marker_path(stale_dir), "pid=not-a-pid\n"),
        k_failure_prefix, "create private malformed marker");

    sintra::cleanup_stale_swarm_directories(cleanup_base, current_pid, current_start);
    if (stale_directory_cleanup_runs()) {
        sintra::test::require_true(!std::filesystem::exists(stale_dir), k_failure_prefix,
            "cleanup_stale_swarm_directories should remove malformed marker directories");
    }
}

// A run marker of a live PID with another start stamp belongs to an earlier
// incarnation, except on FreeBSD, where a missed boot-time change can make the
// stamps of one live process differ. There the directory stays until the
// process holding the PID exits.
void test_stale_directory_start_stamp()
{
#if defined(__FreeBSD__)
    constexpr bool k_mismatch_is_stale = false;
#else
    constexpr bool k_mismatch_is_stale = true;
#endif
    const auto current_pid   = static_cast<std::uint32_t>(sintra::get_current_pid());
    const auto current_start = sintra::current_process_start_stamp().value_or(0);
    const auto cleanup_base  = sintra::test::unique_scratch_directory("utility_stamp_cleanup") / "private";
    sintra::test::require_true(sintra::detail::create_private_directory(cleanup_base),
        k_failure_prefix, "create private cleanup root");

    auto write_marker = [&](const char* name, std::uint32_t pid, std::uint64_t start_stamp) {
        const auto directory = cleanup_base / name;
        sintra::test::require_true(sintra::detail::create_private_directory(directory),
            k_failure_prefix, "create private run directory");
        sintra::run_marker_record_t record{};
        record.pid                  = pid;
        record.start_stamp          = start_stamp;
        record.created_monotonic_ns = sintra::monotonic_now_ns();
        sintra::test::require_true(sintra::write_run_marker(directory, record), k_failure_prefix,
            "write_run_marker should write into an existing directory");
        return directory;
    };

    sintra::test::require_true(current_start != 0, k_failure_prefix,
        "the current process must have a start stamp");
    const auto own_pid_dir = write_marker("own_pid", current_pid, current_start + 1);
#ifndef _WIN32
    int release_pipe[2];
    sintra::test::require_true(::pipe(release_pipe) == 0, k_failure_prefix, "pipe should succeed");
    const pid_t child_pid = ::fork();
    sintra::test::require_true(child_pid >= 0, k_failure_prefix, "fork should succeed");
    if (child_pid == 0) {
        ::close(release_pipe[1]);
        char release = 0;
        while (::read(release_pipe[0], &release, 1) < 0 && errno == EINTR) {}
        ::_exit(0);
    }
    ::close(release_pipe[0]);
    const auto child_start = sintra::query_process_start_stamp(static_cast<std::uint32_t>(child_pid));
    const auto live_pid_dir = write_marker(
        "live_pid", static_cast<std::uint32_t>(child_pid), child_start.value_or(0) + 1);
#endif

    sintra::cleanup_stale_swarm_directories(cleanup_base, current_pid, current_start);
    if (stale_directory_cleanup_runs()) {
        sintra::test::require_true(std::filesystem::exists(own_pid_dir) != k_mismatch_is_stale,
            k_failure_prefix,
            "a marker of this PID with another start stamp is stale everywhere except on FreeBSD");
    }
#ifndef _WIN32
    const bool live_pid_kept = std::filesystem::exists(live_pid_dir);
    ::close(release_pipe[1]);
    int   child_status = 0;
    pid_t waited       = 0;
    do {
        waited = ::waitpid(child_pid, &child_status, 0);
    }
    while (waited < 0 && errno == EINTR);
    sintra::test::require_true(child_start.has_value() && waited == child_pid, k_failure_prefix,
        "the marker child must have a start stamp and be reaped");
    sintra::test::require_true(live_pid_kept != k_mismatch_is_stale, k_failure_prefix,
        "a marker of a live PID with another start stamp is stale everywhere except on FreeBSD");

    sintra::cleanup_stale_swarm_directories(cleanup_base, current_pid, current_start);
    sintra::test::require_true(!std::filesystem::exists(live_pid_dir), k_failure_prefix,
        "a marker whose process has exited must be stale");
#endif
}

// A live process other than this one: the test's parent.
std::uint32_t live_foreign_pid()
{
#ifdef _WIN32
    const DWORD self = ::GetCurrentProcessId();
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    sintra::test::require_true(snapshot != INVALID_HANDLE_VALUE, k_failure_prefix,
        "CreateToolhelp32Snapshot should succeed");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    for (BOOL found = ::Process32FirstW(snapshot, &entry);
        found;
        found = ::Process32NextW(snapshot, &entry))
    {
        if (entry.th32ProcessID == self) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    ::CloseHandle(snapshot);
    return static_cast<std::uint32_t>(parent);
#else
    return static_cast<std::uint32_t>(::getppid());
#endif
}

// A swarm publishes its run marker after a cleanup scan began.
struct late_marker_t
{
    std::filesystem::path directory;
    std::uint32_t         pid         = 0;
    std::uint64_t         start_stamp = 0;
};

late_marker_t s_late_marker;

void publish_late_marker(const std::filesystem::path&)
{
    sintra::test::require_true(sintra::detail::create_private_directory(s_late_marker.directory),
        k_failure_prefix, "create the late swarm's private directory");
    sintra::run_marker_record_t record{};
    record.pid                  = s_late_marker.pid;
    record.start_stamp          = s_late_marker.start_stamp;
    record.created_monotonic_ns = sintra::monotonic_now_ns();
    sintra::test::require_true(
        sintra::write_run_marker(s_late_marker.directory, record), k_failure_prefix,
        "write_run_marker should write the late marker");
}

// Scans the base directory while a live swarm publishes the late marker, and
// reports whether its directory was kept.
bool late_marker_survives_scan(const std::filesystem::path& base_dir)
{
    sintra::detail::test_hooks::s_swarm_directory_scan_started = publish_late_marker;
    sintra::cleanup_stale_swarm_directories(
        base_dir,
        static_cast<std::uint32_t>(sintra::get_current_pid()),
        sintra::current_process_start_stamp().value_or(0));
    sintra::detail::test_hooks::s_swarm_directory_scan_started = nullptr;
    return std::filesystem::exists(s_late_marker.directory);
}

// A marker created later than the scan's clock reading was created before a
// reboot. The scan reads the clock after each marker, so a live swarm whose
// marker appears after the scan began is never mistaken for one, even while
// its start stamp is unavailable.
void test_marker_published_during_scan()
{
    if (!stale_directory_cleanup_runs()) {
        return;
    }
    const auto base_dir = sintra::test::unique_scratch_directory("utility_scan_race") / "private";
    sintra::test::require_true(sintra::detail::create_private_directory(base_dir),
        k_failure_prefix, "create private cleanup root");
    const auto live_pid = live_foreign_pid();
    sintra::test::require_true(live_pid != 0 && sintra::is_process_alive(live_pid), k_failure_prefix,
        "the test's parent must be a live process");

    s_late_marker = {base_dir / "without_stamp", live_pid, 0};
    sintra::test::require_true(late_marker_survives_scan(base_dir), k_failure_prefix,
        "a live swarm without a start stamp whose marker appears during the scan must be kept");

#if defined(__FreeBSD__)
    // The boot time moves during every attempt, so the live process's start
    // stamp is unavailable while the scan runs.
    const auto parent_stamp = sintra::query_process_start_stamp(live_pid);
    sintra::test::require_true(parent_stamp.has_value(), k_failure_prefix,
        "the parent process must have a start stamp");
    s_late_marker = {base_dir / "stamp_unavailable", live_pid, *parent_stamp};
    {
        namespace fakes = sintra::test::identity_fakes;
        fakes::Scoped_fakes injected;
        fakes::s_moving_boot_time_lookups = fakes::k_every_lookup;
        sintra::test::require_true(late_marker_survives_scan(base_dir), k_failure_prefix,
            "a live swarm with an unavailable start stamp and a late marker must be kept");
    }
#endif

    // A marker from before a reboot still reads later than the clock.
    const auto rebooted_dir = base_dir / "before_reboot";
    sintra::test::require_true(sintra::detail::create_private_directory(rebooted_dir),
        k_failure_prefix, "create the earlier boot's private directory");
    sintra::run_marker_record_t rebooted{};
    rebooted.pid                  = live_pid;
    rebooted.created_monotonic_ns = sintra::monotonic_now_ns() + 3'600'000'000'000ull;
    sintra::test::require_true(sintra::write_run_marker(rebooted_dir, rebooted), k_failure_prefix,
        "write_run_marker should write the earlier-boot marker");
    sintra::cleanup_stale_swarm_directories(
        base_dir,
        static_cast<std::uint32_t>(sintra::get_current_pid()),
        sintra::current_process_start_stamp().value_or(0));
    sintra::test::require_true(!std::filesystem::exists(rebooted_dir), k_failure_prefix,
        "a marker created later than the scan's clock reading must be stale");
}

// A run marker is published in one step. While its contents are being written,
// a scan finds no marker under its name, never a partial one, and keeps the
// directory; afterwards the marker is complete and nothing staged remains.
void test_marker_publication_is_atomic()
{
    if (!stale_directory_cleanup_runs()) {
        return;
    }
    const auto current_pid   = static_cast<std::uint32_t>(sintra::get_current_pid());
    const auto current_start = sintra::current_process_start_stamp().value_or(0);
    const auto base_dir = sintra::test::unique_scratch_directory("utility_marker_publication") / "private";
    const auto run_dir  = base_dir / "publishing";
    sintra::test::require_true(
        sintra::detail::create_private_directory(base_dir) &&
            sintra::detail::create_private_directory(run_dir),
        k_failure_prefix, "create private run directories");

    sintra::run_marker_record_t record{};
    record.pid                  = current_pid;
    record.start_stamp          = current_start;
    record.created_monotonic_ns = sintra::monotonic_now_ns();
    record.recovery_occurrence  = 3;

    bool staged_complete = false;
    bool marker_absent   = false;
    bool kept_while_staged = false;
    sintra::detail::after_private_file_staged_for_test =
        [&](const std::filesystem::path& staged, const std::filesystem::path& path) {
            const auto staged_record = sintra::read_run_marker(staged);
            staged_complete = staged_record &&
                staged_record->created_monotonic_ns == record.created_monotonic_ns &&
                staged_record->recovery_occurrence == record.recovery_occurrence;
            marker_absent = !std::filesystem::exists(path);
            sintra::cleanup_stale_swarm_directories(base_dir, current_pid, current_start);
            kept_while_staged = std::filesystem::exists(run_dir);
        };
    const bool published = sintra::write_run_marker(run_dir, record);
    sintra::detail::after_private_file_staged_for_test = nullptr;

    sintra::test::require_true(published && staged_complete, k_failure_prefix,
        "the marker's contents must be written in full before it is published");
    sintra::test::require_true(marker_absent && kept_while_staged, k_failure_prefix,
        "a scan during publication must find no marker and keep the directory");
    const auto marker = sintra::read_run_marker(sintra::run_marker_path(run_dir));
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(run_dir)) {
        (void)entry;
        ++entries;
    }
    sintra::test::require_true(
        marker && marker->created_monotonic_ns == record.created_monotonic_ns && entries == 1,
        k_failure_prefix,
        "the published marker must be complete, with no staged file left behind");
}

#ifdef _WIN32
void test_marker_read_during_delete_access_window()
{
    if (!stale_directory_cleanup_runs()) {
        return;
    }

    const auto current_pid   = static_cast<std::uint32_t>(sintra::get_current_pid());
    const auto current_start = sintra::current_process_start_stamp().value_or(0);
    const auto base_dir = sintra::test::unique_scratch_directory("utility_marker_delete_share") / "private";
    const auto run_dir  = base_dir / "publishing";
    sintra::test::require_true(
        sintra::detail::create_private_directory(base_dir) &&
            sintra::detail::create_private_directory(run_dir),
        k_failure_prefix, "create private run directories");

    sintra::run_marker_record_t record{};
    record.pid                  = current_pid;
    record.start_stamp          = current_start;
    record.created_monotonic_ns = sintra::monotonic_now_ns();
    sintra::test::require_true(sintra::write_run_marker(run_dir, record),
        k_failure_prefix, "publish run marker");
    const auto marker_path = sintra::run_marker_path(run_dir);

    const HANDLE shared_delete = ::CreateFileW(marker_path.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    sintra::test::require_true(shared_delete != INVALID_HANDLE_VALUE,
        k_failure_prefix, "open marker with delete access and sharing");
    const auto read_record = sintra::read_run_marker(marker_path);
    const bool shared_closed = ::CloseHandle(shared_delete) != 0;
    sintra::test::require_true(shared_closed && read_record && read_record->pid == current_pid,
        k_failure_prefix, "read a marker while another handle has delete access");

    const HANDLE exclusive_delete = ::CreateFileW(marker_path.c_str(), DELETE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    sintra::test::require_true(exclusive_delete != INVALID_HANDLE_VALUE,
        k_failure_prefix, "open marker with exclusive delete access");
    bool read_succeeded = true;
    const auto unreadable = sintra::read_run_marker(marker_path, &read_succeeded);
    sintra::cleanup_stale_swarm_directories(base_dir, current_pid, current_start);
    const bool kept_during_open = std::filesystem::exists(run_dir) &&
        !std::filesystem::exists(sintra::run_marker_cleanup_path(run_dir));
    const bool exclusive_closed = ::CloseHandle(exclusive_delete) != 0;
    sintra::cleanup_stale_swarm_directories(base_dir, current_pid, current_start);
    sintra::test::require_true(exclusive_closed && !read_succeeded && !unreadable && kept_during_open &&
        std::filesystem::exists(run_dir) && std::filesystem::exists(marker_path),
        k_failure_prefix, "a temporarily unreadable marker must not mark or delete a live directory");
}
#endif

#if defined(__linux__)
char linux_leader_state(pid_t pid)
{
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    std::string   stat_line;
    std::getline(stat_file, stat_line);

    const auto closing_paren = stat_line.rfind(')');
    if (closing_paren == std::string::npos || closing_paren + 2 >= stat_line.size()) {
        return '\0';
    }
    return stat_line[closing_paren + 2];
}

// A process lives while any of its threads runs. A thread-group leader that
// exits first stays a zombie until the other threads exit, so its state alone
// does not show that the process has exited.
void test_process_alive_after_main_thread_exit()
{
    int release_pipe[2];
    sintra::test::require_true(::pipe(release_pipe) == 0, k_failure_prefix,
        "pipe should succeed for the main-thread exit liveness check");

    const pid_t child_pid = ::fork();
    sintra::test::require_true(child_pid >= 0, k_failure_prefix,
        "fork should succeed for the main-thread exit liveness check");

    if (child_pid == 0) {
        ::close(release_pipe[1]);
        const int release_fd = release_pipe[0];
        std::thread([release_fd]() {
            char released = 0;
            while (::read(release_fd, &released, 1) < 0 && errno == EINTR) {}
            ::_exit(0);
        }).detach();
        ::pthread_exit(nullptr);
    }
    ::close(release_pipe[0]);

    // Closing the pipe ends the remaining child thread, which exits the process.
    struct Child_process
    {
        pid_t m_pid;
        int   m_release_fd;

        void release()
        {
            if (m_release_fd >= 0) {
                ::close(m_release_fd);
                m_release_fd = -1;
            }
        }

        int reap()
        {
            release();
            int   status = 0;
            pid_t waited = 0;
            do {
                waited = ::waitpid(m_pid, &status, 0);
            }
            while (waited < 0 && errno == EINTR);
            m_pid = -1;
            return waited < 0 ? -1 : status;
        }

        ~Child_process()
        {
            if (m_pid > 0) {
                reap();
            }
        }
    } child{child_pid, release_pipe[1]};

    bool leader_exited = false;
    for (int attempt = 0; attempt < 2000 && !leader_exited; ++attempt) {
        leader_exited = linux_leader_state(child_pid) == 'Z';
        if (!leader_exited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    sintra::test::require_true(leader_exited, k_failure_prefix,
        "the child's main thread should exit while its other thread runs");
    sintra::test::require_true(
        sintra::is_process_alive(static_cast<std::uint32_t>(child_pid)),
        k_failure_prefix,
        "a process whose main thread exited should stay alive while another thread runs");

    child.release();
    bool process_reported_dead = false;
    for (int attempt = 0; attempt < 2000 && !process_reported_dead; ++attempt) {
        process_reported_dead = !sintra::is_process_alive(static_cast<std::uint32_t>(child_pid));
        if (!process_reported_dead) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const int child_status = child.reap();
    sintra::test::require_true(process_reported_dead, k_failure_prefix,
        "a process should be reported dead once its last thread exits");
    sintra::test::require_true(
        WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
        k_failure_prefix,
        "the main-thread exit child should exit normally");
}
#endif

} // namespace

int main()
{
    try {
        test_adaptive_function_basic();
        test_adaptive_function_copy_constructor();
        test_adaptive_function_copy_assignment();
        test_cstring_vector_from_lvalue();
        test_cstring_vector_from_rvalue();
        test_cstring_vector_empty();
        test_env_key_of();
#ifndef _WIN32
        test_build_environment_entries();
#else
        test_windows_environment_block();
#endif
        test_spinlocked_umap_scoped_erase();
        test_process_utility_helpers();
        test_stale_directory_start_stamp();
        test_marker_published_during_scan();
        test_marker_publication_is_atomic();
#ifdef _WIN32
        test_marker_read_during_delete_access_window();
#endif
#if defined(__linux__)
        test_process_alive_after_main_thread_exit();
#endif
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "utility_test failed: %s\n", ex.what());
        return 1;
    }

    std::fprintf(stderr, "utility_test passed\n");
    return 0;
}
