// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include "../time_utils.h"
#include "platform_defs.h"

#ifdef _WIN32
  #include "../sintra_windows.h"
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  #if defined(__FreeBSD__)
    #include <sys/sysctl.h>
    #include <sys/user.h>
  #elif defined(__APPLE__)
    #include <libproc.h>
    #include <sys/sysctl.h>
  #endif
#endif

namespace sintra {

namespace detail {

// Optional hooks for test builds; override via macros before including sintra.
#ifndef SINTRA_PRESERVE_SCRATCH
#define SINTRA_PRESERVE_SCRATCH 0
#endif

#ifndef SINTRA_TEST_ROOT
#define SINTRA_TEST_ROOT nullptr
#endif

#if defined(__APPLE__) && !defined(_WIN32)
inline constexpr int k_macos_process_status_zombie = 5; // SZOMB in <sys/proc.h>

inline std::optional<bool> macos_process_is_exited_or_zombie(uint32_t pid)
{
    struct proc_bsdinfo bsd_info;
    std::memset(&bsd_info, 0, sizeof(bsd_info));

    errno = 0;
    const int proc_result = ::proc_pidinfo(
        static_cast<int>(pid),
        PROC_PIDTBSDINFO,
        0,
        &bsd_info,
        sizeof(bsd_info));
    if (proc_result > 0 && static_cast<size_t>(proc_result) >= sizeof(bsd_info)) {
        return bsd_info.pbi_status == k_macos_process_status_zombie;
    }
    if (proc_result <= 0 && errno == ESRCH) {
        return true;
    }

    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc kip;
    std::memset(&kip, 0, sizeof(kip));
    size_t len = sizeof(kip);

    errno = 0;
    if (::sysctl(mib, 4, &kip, &len, nullptr, 0) == 0) {
        if (len == 0) {
            return true;
        }
        if (len < sizeof(kip)) {
            return std::nullopt;
        }
        return kip.kp_proc.p_stat == k_macos_process_status_zombie;
    }
    if (errno == ESRCH) {
        return true;
    }

    return std::nullopt;
}
#endif

} // namespace detail

inline bool is_process_alive(uint32_t pid)
{
#ifdef _WIN32
    if (pid == 0) {
        return false;
    }

    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            return true;
        }
        return false;
    }

    DWORD code  = 0;
    bool  alive = false;
    if (::GetExitCodeProcess(h, &code)) {
        alive = (code == STILL_ACTIVE);
    }
    ::CloseHandle(h);
    return alive;
#else
    if (!pid) {
        return false;
    }

    if (::kill(static_cast<pid_t>(pid), 0) != 0) {
        return errno != ESRCH;
    }

#if defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc kip;
    std::memset(&kip, 0, sizeof(kip));
    size_t len = sizeof(kip);
    if (::sysctl(mib, 4, &kip, &len, nullptr, 0) != 0) {
        return true;
    }

    if (len == 0) {
        return false;
    }

    switch (kip.ki_stat) {
        case SZOMB:
#ifdef SDEAD
        case SDEAD:
#endif
            return false;
        default:
            return true;
    }
#elif defined(__APPLE__)
    const auto exited_or_zombie = detail::macos_process_is_exited_or_zombie(pid);
    if (exited_or_zombie) {
        return !*exited_or_zombie;
    }
    return true;
#else
    std::ifstream stat_file;
    stat_file.open(std::string("/proc/") + std::to_string(pid) + "/stat");
    if (!stat_file.is_open()) {
        return true;
    }

    std::string stat_line;
    std::getline(stat_file, stat_line);

    auto closing_paren = stat_line.rfind(')');
    if (closing_paren == std::string::npos) {
        return true;
    }

    auto state_pos = stat_line.find_first_not_of(' ', closing_paren + 1);
    if (state_pos == std::string::npos) {
        return true;
    }

    char state = stat_line[state_pos];
    if (state != 'Z' && state != 'X') {
        return true;
    }

    // The state is the thread-group leader's. A leader that exits before the
    // other threads stays a zombie until they have exited too, and num_threads
    // (field 20) keeps counting it until the process is reaped. The process
    // has therefore exited only when the leader is the one thread left.
    std::istringstream fields(stat_line.substr(state_pos + 1));
    std::string skipped_field;
    for (int field = 4; field < 20; ++field) {
        if (!(fields >> skipped_field)) {
            return true;
        }
    }

    unsigned long num_threads = 0;
    if (!(fields >> num_threads)) {
        return true;
    }
    return num_threads > 1;
#endif
#endif
}

inline std::optional<uint64_t> query_process_start_stamp(uint32_t pid)
{
#ifdef _WIN32
    if (pid == 0) {
        return std::nullopt;
    }

    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            return std::nullopt;
        }
        return std::nullopt;
    }

    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!::GetProcessTimes(h, &creation, &exit, &kernel, &user)) {
        ::CloseHandle(h);
        return std::nullopt;
    }

    ULARGE_INTEGER stamp{};
    stamp.LowPart = creation.dwLowDateTime;
    stamp.HighPart = creation.dwHighDateTime;
    ::CloseHandle(h);
    return stamp.QuadPart;
#elif defined(__APPLE__)
    if (pid == 0) {
        return std::nullopt;
    }

    struct proc_bsdinfo bsd_info;
    std::memset(&bsd_info, 0, sizeof(bsd_info));
    int result = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &bsd_info, sizeof(bsd_info));
    if (result <= 0 || static_cast<size_t>(result) < sizeof(bsd_info)) {
        return std::nullopt;
    }

    const uint64_t seconds = static_cast<uint64_t>(bsd_info.pbi_start_tvsec);
    const uint64_t usec    = static_cast<uint64_t>(bsd_info.pbi_start_tvusec);
    return seconds * 1000000000ull + usec * 1000ull;
#elif defined(__FreeBSD__)
    if (pid == 0) {
        return std::nullopt;
    }

    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc kip;
    std::memset(&kip, 0, sizeof(kip));
    size_t len = sizeof(kip);
    if (::sysctl(mib, 4, &kip, &len, nullptr, 0) != 0 || len != sizeof(kip)) {
        return std::nullopt;
    }

    const uint64_t seconds = static_cast<uint64_t>(kip.ki_start.tv_sec);
    const uint64_t usec    = static_cast<uint64_t>(kip.ki_start.tv_usec);
    return seconds * 1000000000ull + usec * 1000ull;
#elif defined(__linux__)
    if (pid == 0) {
        return std::nullopt;
    }

    std::ifstream stat_file(std::string("/proc/") + std::to_string(pid) + "/stat");
    if (!stat_file.is_open()) {
        return std::nullopt;
    }

    std::string stat_line;
    std::getline(stat_file, stat_line);
    stat_file.close();

    auto closing_paren = stat_line.rfind(')');
    if (closing_paren == std::string::npos) {
        return std::nullopt;
    }

    std::istringstream iss(stat_line.substr(closing_paren + 1));
    std::string token;

    if (!(iss >> token)) { // state
        return std::nullopt;
    }

    for (int i = 0; i < 18; ++i) {
        if (!(iss >> token)) {
            return std::nullopt;
        }
    }

    if (!(iss >> token)) {
        return std::nullopt;
    }

    try {
        return static_cast<uint64_t>(std::stoull(token));
    }
    catch (...) {
        return std::nullopt;
    }
#else
    (void)pid;
    return std::nullopt;
#endif
}

inline std::optional<uint64_t> current_process_start_stamp()
{
    return query_process_start_stamp(get_current_pid());
}

enum class Process_identity_status
{
    LIVE,
    DEAD,
    UNKNOWN
};

struct process_identity_result_t
{
    Process_identity_status status;
    std::error_code error;
};

namespace detail {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
using process_identity_probe_hook_t = process_identity_result_t (*)(uint32_t, uint64_t);
inline process_identity_probe_hook_t process_identity_probe_hook = nullptr;
#ifdef _WIN32
inline decltype(&::OpenProcess) process_identity_open_process = ::OpenProcess;
inline decltype(&::GetProcessTimes) process_identity_get_process_times = ::GetProcessTimes;
inline decltype(&::WaitForSingleObject) process_identity_wait_for_single_object = ::WaitForSingleObject;
#endif
#endif

inline process_identity_result_t unknown_process_identity(int error)
{
    return {Process_identity_status::UNKNOWN, std::error_code(error, std::system_category())};
}

#if defined(__linux__)
struct linux_process_stat_t
{
    char state = '\0';
    uint64_t num_threads = 0;
    uint64_t start_stamp = 0;
};

inline bool parse_linux_process_stat(const std::string& stat_line, linux_process_stat_t& result)
{
    // The command name can contain spaces and parentheses. The fields after
    // its final ')' contain state, thread count and incarnation in one record.
    const auto closing_paren = stat_line.rfind(')');
    if (closing_paren == std::string::npos) {
        return false;
    }

    std::istringstream fields(stat_line.substr(closing_paren + 1));
    std::string skipped;
    if (!(fields >> result.state)) {
        return false;
    }
    for (int field = 4; field < 20; ++field) {
        if (!(fields >> skipped)) {
            return false;
        }
    }
    return (fields >> result.num_threads >> skipped >> result.start_stamp) &&
        result.num_threads != 0 && result.start_stamp != 0;
}
#endif

// Observe one native process incarnation. Observation errors never authorize
// release of memory still protected by its reader slot.
inline process_identity_result_t probe_process_identity_native(uint32_t pid, uint64_t start_stamp)
{
#ifdef _WIN32
    if (pid == 0 || start_stamp == 0) {
        return unknown_process_identity(ERROR_INVALID_PARAMETER);
    }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    HANDLE process = process_identity_open_process(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
#else
    HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
#endif
    if (!process) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            return {Process_identity_status::DEAD, {}};
        }
        return unknown_process_identity(error);
    }

    FILETIME creation{}, exit{}, kernel{}, user{};
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const BOOL queried = process_identity_get_process_times(process, &creation, &exit, &kernel, &user);
#else
    const BOOL queried = ::GetProcessTimes(process, &creation, &exit, &kernel, &user);
#endif
    if (!queried) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(process);
        return unknown_process_identity(error);
    }

    ULARGE_INTEGER observed_stamp{};
    observed_stamp.LowPart = creation.dwLowDateTime;
    observed_stamp.HighPart = creation.dwHighDateTime;
    if (observed_stamp.QuadPart != start_stamp) {
        ::CloseHandle(process);
        return {Process_identity_status::DEAD, {}};
    }

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const DWORD wait_result = process_identity_wait_for_single_object(process, 0);
#else
    const DWORD wait_result = ::WaitForSingleObject(process, 0);
#endif
    const DWORD error = wait_result == WAIT_FAILED ? ::GetLastError() : ERROR_INVALID_DATA;
    ::CloseHandle(process);
    if (wait_result == WAIT_OBJECT_0) {
        return {Process_identity_status::DEAD, {}};
    }
    if (wait_result == WAIT_TIMEOUT) {
        return {Process_identity_status::LIVE, {}};
    }
    return unknown_process_identity(error);
#else
    if (pid == 0 || pid > static_cast<uint32_t>(INT32_MAX) || start_stamp == 0) {
        return unknown_process_identity(EINVAL);
    }
#if defined(__linux__)
    const auto path = std::string("/proc/") + std::to_string(pid) + "/stat";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int error = errno;
        // A missing procfs mount is not process death. Only ESRCH from the
        // kernel establishes absence when the expected record cannot be opened.
        if (error == ESRCH || (error == ENOENT && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH)) {
            return {Process_identity_status::DEAD, {}};
        }
        return unknown_process_identity(error);
    }

    char buffer[4096];
    const auto size = ::read(fd, buffer, sizeof(buffer));
    const int error = errno;
    ::close(fd);
    if (size < 0) {
        if (error == ESRCH) {
            return {Process_identity_status::DEAD, {}};
        }
        return unknown_process_identity(error);
    }
    if (size == sizeof(buffer)) {
        return unknown_process_identity(EOVERFLOW);
    }

    linux_process_stat_t record;
    if (!parse_linux_process_stat(std::string(buffer, static_cast<size_t>(size)), record)) {
        return unknown_process_identity(EIO);
    }
    if (record.start_stamp != start_stamp ||
        ((record.state == 'Z' || record.state == 'X') && record.num_threads == 1))
    {
        return {Process_identity_status::DEAD, {}};
    }
    return {Process_identity_status::LIVE, {}};
#elif defined(__APPLE__)
    struct proc_bsdinfo record{};
    errno = 0;
    const int size = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &record, sizeof(record));
    if (size <= 0 && errno == ESRCH) {
        return {Process_identity_status::DEAD, {}};
    }
    if (size != sizeof(record)) {
        return unknown_process_identity(errno ? errno : EIO);
    }
    const uint64_t observed_stamp = static_cast<uint64_t>(record.pbi_start_tvsec) * 1000000000ull +
        static_cast<uint64_t>(record.pbi_start_tvusec) * 1000ull;
    if (observed_stamp == 0) {
        return unknown_process_identity(EIO);
    }
    if (observed_stamp != start_stamp || record.pbi_status == k_macos_process_status_zombie) {
        return {Process_identity_status::DEAD, {}};
    }
    return {Process_identity_status::LIVE, {}};
#elif defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc record{};
    size_t size = sizeof(record);
    if (::sysctl(mib, 4, &record, &size, nullptr, 0) != 0) {
        const int error = errno;
        if (error == ESRCH) {
            return {Process_identity_status::DEAD, {}};
        }
        return unknown_process_identity(error);
    }
    if (size == 0) {
        return {Process_identity_status::DEAD, {}};
    }
    if (size != sizeof(record)) {
        return unknown_process_identity(EIO);
    }
    const uint64_t observed_stamp = static_cast<uint64_t>(record.ki_start.tv_sec) * 1000000000ull +
        static_cast<uint64_t>(record.ki_start.tv_usec) * 1000ull;
    if (observed_stamp == 0) {
        return unknown_process_identity(EIO);
    }
    if (observed_stamp != start_stamp || record.ki_stat == SZOMB
#ifdef SDEAD
        || record.ki_stat == SDEAD
#endif
    ) {
        return {Process_identity_status::DEAD, {}};
    }
    return {Process_identity_status::LIVE, {}};
#else
    return unknown_process_identity(ENOTSUP);
#endif
#endif
}

} // namespace detail

inline process_identity_result_t probe_process_identity(uint32_t pid, uint64_t start_stamp)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (detail::process_identity_probe_hook) {
        return detail::process_identity_probe_hook(pid, start_stamp);
    }
#endif
    return detail::probe_process_identity_native(pid, start_stamp);
}

struct run_marker_record_t
{
    uint32_t   pid                  = 0;
    uint64_t   start_stamp          = 0;
    uint64_t   created_monotonic_ns = 0;
    uint32_t   recovery_occurrence  = 0;
};

inline const char* run_marker_filename()
{
    return "sintra_run.marker";
}

inline const char* run_marker_cleanup_suffix()
{
    return ".cleanup";
}

inline std::filesystem::path run_marker_path(const std::filesystem::path& directory)
{
    return directory / run_marker_filename();
}

inline std::filesystem::path run_marker_cleanup_path(const std::filesystem::path& directory)
{
    return directory / (std::string(run_marker_filename()) + run_marker_cleanup_suffix());
}

inline bool write_run_marker(
    const std::filesystem::path&   directory,
    const run_marker_record_t&     record)
{
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return false;
    }

    std::ofstream marker(run_marker_path(directory), std::ios::out | std::ios::trunc);
    if (!marker.is_open()) {
        return false;
    }

    marker << "pid=" << record.pid << '\n';
    marker << "start_ns=" << record.start_stamp << '\n';
    marker << "created_ns=" << record.created_monotonic_ns << '\n';
    marker << "occurrence=" << record.recovery_occurrence << '\n';
    marker.close();
    return static_cast<bool>(marker);
}

inline std::optional<run_marker_record_t> read_run_marker(const std::filesystem::path& marker_path)
{
    std::ifstream marker(marker_path);
    if (!marker.is_open()) {
        return std::nullopt;
    }

    run_marker_record_t record;
    std::string line;
    while (std::getline(marker, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        auto key   = line.substr(0, pos);
        auto value = line.substr(pos + 1);

        try {
            if (key == "pid")        { record.pid = static_cast<uint32_t>(std::stoul(value)); } else
            if (key == "start_ns")   { record.start_stamp = std::stoull(value);               } else
            if (key == "created_ns") { record.created_monotonic_ns = std::stoull(value);      } else
            if (key == "occurrence") { record.recovery_occurrence = static_cast<uint32_t>(std::stoul(value)); }
        }
        catch (...) {
            return std::nullopt;
        }
    }

    if (record.pid == 0) {
        return std::nullopt;
    }

    return record;
}

inline void remove_run_marker_files(const std::filesystem::path& directory)
{
    std::error_code ec;
    std::filesystem::remove(run_marker_path(directory), ec);
    ec.clear();
    std::filesystem::remove(run_marker_cleanup_path(directory), ec);
}

inline void mark_run_directory_for_cleanup(const std::filesystem::path& directory)
{
    const auto marker  = run_marker_path(directory);
    const auto cleanup = run_marker_cleanup_path(directory);

    std::error_code exists_ec;
    if (std::filesystem::exists(cleanup, exists_ec)) {
        return;
    }

    auto record_opt = read_run_marker(marker);

    std::error_code rename_ec;
    std::filesystem::rename(marker, cleanup, rename_ec);
    if (!rename_ec) {
        return;
    }

    std::ofstream cleanup_file(cleanup, std::ios::out | std::ios::trunc);
    if (cleanup_file.is_open()) {
        if (record_opt) {
            const auto& record = *record_opt;
            cleanup_file << "pid=" << record.pid << '\n';
            cleanup_file << "start_ns=" << record.start_stamp << '\n';
            cleanup_file << "created_ns=" << record.created_monotonic_ns << '\n';
            cleanup_file << "occurrence=" << record.recovery_occurrence << '\n';
        }
        cleanup_file.close();
    }

    std::error_code remove_marker_ec;
    std::filesystem::remove(marker, remove_marker_ec);
}

#ifdef _WIN32
inline bool path_has_prefix_ci(
    const std::filesystem::path&   path,
    const std::filesystem::path&   prefix)
{
    const auto  normalized_path   = path.lexically_normal();
    const auto  normalized_prefix = prefix.lexically_normal();
    const auto& path_native       = normalized_path.native();
    const auto& prefix_native     = normalized_prefix.native();

    if (prefix_native.empty() || path_native.size() < prefix_native.size()) {
        return false;
    }

    if (_wcsnicmp(path_native.c_str(), prefix_native.c_str(), prefix_native.size()) != 0) {
        return false;
    }

    if (path_native.size() == prefix_native.size()) {
        return true;
    }

    const wchar_t next = path_native[prefix_native.size()];
    return next == L'\\' || next == L'/';
}
#endif

inline void cleanup_stale_swarm_directories(
    const std::filesystem::path&   base_dir,
    uint32_t                       current_pid,
    uint64_t                       current_start_stamp)
{
    std::error_code ec;
    if (!std::filesystem::exists(base_dir, ec) || !std::filesystem::is_directory(base_dir, ec)) {
        return;
    }

#ifdef _WIN32
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const bool preserve = (SINTRA_PRESERVE_SCRATCH);
    if (preserve) {
        const char* test_root_env = (SINTRA_TEST_ROOT);
        if (!test_root_env || !*test_root_env) {
            return;
        }

        std::error_code root_ec;
        std::error_code base_ec;
        const auto test_root = std::filesystem::weakly_canonical(
            std::filesystem::path(test_root_env), root_ec);
        const auto base_path = std::filesystem::weakly_canonical(base_dir, base_ec);
        if (root_ec || base_ec || !path_has_prefix_ci(base_path, test_root)) {
            return;
        }
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

    const auto now_monotonic = monotonic_now_ns();

    for (std::filesystem::directory_iterator it(base_dir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
        std::error_code status_ec;
        if (!it->is_directory(status_ec)) {
            continue;
        }

        const auto& dir_path     = it->path();
        const auto  marker_path  = run_marker_path(dir_path);
        const auto  cleanup_path = run_marker_cleanup_path(dir_path);

        std::error_code exists_ec;
        const bool has_marker = std::filesystem::exists(marker_path, exists_ec);
        exists_ec.clear();
        const bool has_cleanup = std::filesystem::exists(cleanup_path, exists_ec);

        if (!has_marker && !has_cleanup) {
            continue;
        }

        auto record_opt = read_run_marker(has_marker ? marker_path : cleanup_path);
        bool stale      = has_cleanup;

        if (!record_opt) {
            stale = true;
        }
        else {
            const auto& record = *record_opt;
            if (record.pid == current_pid) {
                if (record.start_stamp != 0 && current_start_stamp != 0 && record.start_stamp != current_start_stamp) {
                    stale = true;
                }
                else {
                    continue;
                }
            }
            else {
                const bool alive = is_process_alive(record.pid);
                if (!alive) {
                    stale = true;
                }
                else
                if (record.start_stamp != 0) {
                    auto running_start = query_process_start_stamp(record.pid);
                    if (running_start && *running_start != record.start_stamp) {
                        stale = true;
                    }
                    else
                    if (!running_start && record.created_monotonic_ns > now_monotonic) {
                        stale = true;
                    }
                }
                else
                if (record.created_monotonic_ns > now_monotonic) {
                    stale = true;
                }
            }
        }

        if (!stale) {
            continue;
        }

        if (!has_cleanup && has_marker) {
            mark_run_directory_for_cleanup(dir_path);
        }

        std::error_code remove_ec;
        std::filesystem::remove_all(dir_path, remove_ec);
        (void)remove_ec;
    }
}

} // namespace sintra
