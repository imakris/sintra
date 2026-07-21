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

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
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
    sintra::test::require_true(contains_entry(L"Ordinary=preserved"),
        k_failure_prefix,
        "an unrelated ordinary environment entry should be preserved");

    const auto block = build_environment_block({
        "SINTRA_ZZZ_ENVIRONMENT_ORDER=last",
        "=D:=D:\\ordered-d",
        "SINTRA_AAA_ENVIRONMENT_ORDER=first",
        "=C:=C:\\ordered-c"
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
    sintra::test::require_true(
        std::is_sorted(
            block_entries.begin(),
            block_entries.end(),
            [](const std::wstring& lhs, const std::wstring& rhs) {
                return CompareStringOrdinal(
                    lhs.c_str(),
                    -1,
                    rhs.c_str(),
                    -1,
                    TRUE) == CSTR_LESS_THAN;
            }),
        k_failure_prefix,
        "a Windows Unicode environment block should be sorted case-insensitively");
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

    const auto cleanup_base = sintra::test::unique_scratch_directory("utility_process_cleanup");
    const auto stale_dir    = cleanup_base / "stale";
    std::filesystem::create_directories(stale_dir);
    std::ofstream stale_marker(sintra::run_marker_path(stale_dir), std::ios::trunc);
    stale_marker << "pid=not-a-pid\n";
    stale_marker.close();

    sintra::cleanup_stale_swarm_directories(cleanup_base, current_pid, current_start);
#if defined(_WIN32)
    const char* preserve_scratch = std::getenv("SINTRA_PRESERVE_SCRATCH");
    const char* test_root        = std::getenv("SINTRA_TEST_ROOT");
    const bool preserve_without_test_root =
        preserve_scratch && preserve_scratch[0] != '\0' && preserve_scratch[0] != '0' &&
        (!test_root || test_root[0] == '\0');
    if (!preserve_without_test_root)
#endif
    sintra::test::require_true(!std::filesystem::exists(stale_dir), k_failure_prefix,
        "cleanup_stale_swarm_directories should remove malformed marker directories");
}

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
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "utility_test failed: %s\n", ex.what());
        return 1;
    }

    std::fprintf(stderr, "utility_test passed\n");
    return 0;
}
