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
#include <string_view>
#include <system_error>

#include "../time_utils.h"
#include "platform_defs.h"
#include "file_utils.h"

#ifdef _WIN32
  #include "../sintra_windows.h"
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  #if defined(__linux__)
    #include <sys/syscall.h>
  #endif

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

#if defined(__FreeBSD__)
#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline decltype(&::sysctl) process_identity_sysctl = ::sysctl;
#endif

inline int freebsd_sysctl(const int* name, u_int length, void* value, size_t* size)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    return process_identity_sysctl(name, length, value, size, nullptr, 0);
#else
    return ::sysctl(name, length, value, size, nullptr, 0);
#endif
}

// Returns zero, or the errno of the failed lookup.
inline int read_freebsd_boot_time(struct timeval& boot_time)
{
    const int name[2] = {CTL_KERN, KERN_BOOTTIME};
    size_t size = sizeof(boot_time);
    return freebsd_sysctl(name, 2, &boot_time, &size) == 0 ? 0 : errno;
}

// A process record and, when established, the process's start stamp: its
// uptime at fork, in nanoseconds.
struct freebsd_process_record_t
{
    struct kinfo_proc record{};
    size_t            size        = 0;
    uint64_t          start_stamp = 0;
    int               stamp_error = 0; // Zero exactly when start_stamp is established.
};

// ki_start is the uptime at fork plus the kernel's boot-time estimate when the
// record is filled (kern_proc.c, fill_kinfo_proc_only), and a wall-clock step
// or leap second moves that estimate (kern_tc.c). kern.boottime reports the
// same estimate, so while it is unchanged around the record, the difference is
// the uptime at fork. A boot time that moves during every attempt leaves the
// stamp unavailable (EAGAIN).
inline constexpr int k_freebsd_boot_time_attempts = 3;

// Returns zero, or the errno of a failed record lookup.
inline int read_freebsd_process_record(uint32_t pid, freebsd_process_record_t& result)
{
    const int name[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    for (int attempt = 0; attempt < k_freebsd_boot_time_attempts; ++attempt) {
        struct timeval before{};
        struct timeval after{};
        const int before_error = read_freebsd_boot_time(before);
        result.size = sizeof(result.record);
        if (freebsd_sysctl(name, 4, &result.record, &result.size) != 0) {
            return errno;
        }
        if (result.size != sizeof(result.record)) {
            result.stamp_error = EIO;
            return 0;
        }
        result.stamp_error = before_error != 0 ? before_error : read_freebsd_boot_time(after);
        if (result.stamp_error != 0) {
            return 0;
        }
        if (before.tv_sec == after.tv_sec && before.tv_usec == after.tv_usec) {
            const auto& start = result.record.ki_start;
            result.start_stamp = static_cast<uint64_t>(
                (static_cast<int64_t>(start.tv_sec) - static_cast<int64_t>(before.tv_sec)) * 1000000000 +
                (static_cast<int64_t>(start.tv_usec) - static_cast<int64_t>(before.tv_usec)) * 1000);
            return 0;
        }
    }
    result.stamp_error = EAGAIN;
    return 0;
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

    detail::freebsd_process_record_t observed;
    if (detail::read_freebsd_process_record(pid, observed) != 0 || observed.stamp_error != 0) {
        return std::nullopt;
    }
    return observed.start_stamp;
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

// Availability of one piece of process metadata. Neither UNKNOWN nor ABSENT
// is evidence about a process, and a cleared slot field reads as UNKNOWN.
enum class Process_metadata_state : uint8_t
{
    UNKNOWN = 0, // The metadata could not be read.
    ABSENT  = 1, // The kernel does not provide it.
    VALID   = 2
};

// A Linux namespace, identified by the device and inode of its nsfs object.
struct process_namespace_t
{
    Process_metadata_state state  = Process_metadata_state::UNKNOWN;
    uint64_t               device = 0;
    uint64_t               inode  = 0;
};

// The Linux namespaces that give a process's PID and start stamp their
// meaning. Other platforms record none.
struct process_namespaces_t
{
    process_namespace_t pid;
    process_namespace_t time;
};

// One process incarnation, as a reader slot publishes it.
struct process_incarnation_t
{
    uint32_t             pid         = 0;
    uint64_t             start_stamp = 0;
    process_namespaces_t namespaces;
};

namespace detail {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
using process_identity_probe_hook_t = process_identity_result_t (*)(const process_incarnation_t&);
inline process_identity_probe_hook_t process_identity_probe_hook = nullptr;
#ifdef _WIN32
inline decltype(&::OpenProcess) process_identity_open_process = ::OpenProcess;
inline decltype(&::GetProcessTimes) process_identity_get_process_times = ::GetProcessTimes;
inline decltype(&::WaitForSingleObject) process_identity_wait_for_single_object = ::WaitForSingleObject;
#else
inline int (*process_identity_kill)(pid_t, int) = ::kill;
#if defined(__APPLE__)
inline decltype(&::proc_pidinfo) process_identity_proc_pidinfo = ::proc_pidinfo;
#elif defined(__linux__)
inline decltype(&::open) process_identity_open_procfs = ::open;
inline int (*process_identity_stat_namespace)(const char*, struct stat*) = ::stat;
#endif
#endif
#endif

inline process_identity_result_t unknown_process_identity(int error)
{
    return {Process_identity_status::UNKNOWN, std::error_code(error, std::system_category())};
}

#ifndef _WIN32
// A positive-PID signal lookup runs in the caller's PID namespace. Under the
// documented deployment requirement, ESRCH means that no process holds the
// PID. Success or EPERM shows that some process holds it, which does not
// identify the published incarnation. A successful call sets no errno, so the
// failed observation's own error stands.
inline process_identity_result_t confirm_absence_by_signal(pid_t pid, int observation_error)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const int result = process_identity_kill(pid, 0);
#else
    const int result = ::kill(pid, 0);
#endif
    if (result == 0) {
        return unknown_process_identity(observation_error);
    }
    const int error = errno;
    if (error == ESRCH) {
        return {Process_identity_status::DEAD, {}};
    }
    return unknown_process_identity(error);
}
#endif

#if defined(__linux__)
// The error of an UNKNOWN result whose process record is readable but lies in
// coordinates known to differ from those of the published incarnation.
inline constexpr int k_foreign_process_record_error = EXDEV;

inline int open_process_pidfd(pid_t pid)
{
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline int (*process_identity_pidfd_open)(pid_t) = open_process_pidfd;
#endif

// Native lookup establishes absence in the caller's PID namespace, whatever
// procfs shows and whatever namespace metadata is available. Without pidfd
// support, a signal lookup does the same.
inline process_identity_result_t confirm_linux_process_absence(pid_t pid, int observation_error)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const int fd = process_identity_pidfd_open(pid);
#else
    const int fd = open_process_pidfd(pid);
#endif
    if (fd >= 0) {
        ::close(fd);
        return unknown_process_identity(observation_error);
    }
    const int error = errno;
    if (error == ESRCH) {
        return {Process_identity_status::DEAD, {}};
    }
    if (error == ENOSYS) {
        return confirm_absence_by_signal(pid, observation_error);
    }
    return unknown_process_identity(error);
}

inline int open_procfs_file(const char* path)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    return process_identity_open_procfs(path, O_RDONLY | O_CLOEXEC);
#else
    return ::open(path, O_RDONLY | O_CLOEXEC);
#endif
}

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

// Reads one complete process-stat record. Returns zero, or the errno of the
// failed step: ENOENT or ESRCH when no record exists, EOVERFLOW or EIO when
// the record is incomplete.
inline int read_linux_process_stat(const char* path, linux_process_stat_t& record)
{
    const int fd = open_procfs_file(path);
    if (fd < 0) {
        return errno;
    }

    char buffer[4096];
    const auto size = ::read(fd, buffer, sizeof(buffer));
    const int error = errno;
    ::close(fd);
    if (size < 0) {
        return error;
    }
    if (static_cast<size_t>(size) == sizeof(buffer)) {
        return EOVERFLOW;
    }
    return parse_linux_process_stat(std::string(buffer, static_cast<size_t>(size)), record) ? 0 : EIO;
}

// Follows a namespace entry to the namespace object: the link's own inode does
// not identify the namespace. A kernel built without this namespace type has
// no entry.
inline process_namespace_t read_linux_namespace(const char* path)
{
    struct stat status{};
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const int result = process_identity_stat_namespace(path, &status);
#else
    const int result = ::stat(path, &status);
#endif
    if (result != 0) {
        const auto state = errno == ENOENT ? Process_metadata_state::ABSENT : Process_metadata_state::UNKNOWN;
        return {state, 0, 0};
    }
    return {Process_metadata_state::VALID, static_cast<uint64_t>(status.st_dev), static_cast<uint64_t>(status.st_ino)};
}

// Read at every use, never cached: a process can join another time namespace
// between one reader slot's lifetime and the next.
inline process_namespaces_t current_linux_namespaces()
{
    return {read_linux_namespace("/proc/self/ns/pid"), read_linux_namespace("/proc/self/ns/time")};
}

// procfs mounted for an ancestor PID namespace lists this process's ID at each
// level from that namespace down, so NStgid has more than one entry. A missing
// line (Linux before 4.1) or an unreadable file is not evidence of such a view.
inline bool linux_procfs_shows_ancestor_namespace()
{
    const int fd = open_procfs_file("/proc/self/status");
    if (fd < 0) {
        return false;
    }
    std::string status;
    char buffer[4096];
    ssize_t size = 0;
    while ((size = ::read(fd, buffer, sizeof(buffer))) > 0) {
        status.append(buffer, static_cast<size_t>(size));
    }
    ::close(fd);

    constexpr std::string_view k_label = "\nNStgid:";
    const auto label = status.find(k_label);
    if (label == std::string::npos) {
        return false;
    }
    const auto begin = label + k_label.size();
    std::istringstream ids(status.substr(begin, status.find('\n', begin) - begin));
    uint64_t id = 0;
    int entries = 0;
    while (ids >> id) {
        ++entries;
    }
    return entries > 1;
}

inline bool namespaces_contradict(const process_namespace_t& published, const process_namespace_t& observed)
{
    return published.state == Process_metadata_state::VALID &&
        observed.state == Process_metadata_state::VALID &&
        (published.device != observed.device || published.inode != observed.inode);
}

// A process record is death evidence only in the coordinates of the published
// stamp. Its PID is numbered in the procfs view's PID namespace, and its start
// time (stat field 22) includes the observer's time-namespace offset. A known
// contradiction makes the record unusable; missing metadata is none. The
// observer's side is read now, for the context it observes in.
inline bool linux_record_coordinates_contradict(const process_namespaces_t& owner)
{
    const auto observer = current_linux_namespaces();
    return namespaces_contradict(owner.pid, observer.pid) ||
        namespaces_contradict(owner.time, observer.time) ||
        linux_procfs_shows_ancestor_namespace();
}
#endif

} // namespace detail

inline std::optional<uint64_t> current_process_start_stamp()
{
#if defined(__linux__)
    // A self-resolving record describes this process in any procfs view.
    detail::linux_process_stat_t record;
    if (detail::read_linux_process_stat("/proc/self/stat", record) != 0) {
        return std::nullopt;
    }
    return record.start_stamp;
#else
    return query_process_start_stamp(get_current_pid());
#endif
}

// Captures this process's incarnation for one reader-slot acquisition. On
// Linux its namespaces are read afresh, alongside the start stamp.
inline std::optional<process_incarnation_t> current_process_incarnation()
{
    const auto start_stamp = current_process_start_stamp();
    if (!start_stamp || *start_stamp == 0) {
        return std::nullopt;
    }
    process_incarnation_t incarnation{get_current_pid(), *start_stamp, {}};
#if defined(__linux__)
    incarnation.namespaces = detail::current_linux_namespaces();
#endif
    return incarnation;
}

namespace detail {

// Observe one native process incarnation. Absence comes from native PID
// lookup; a different incarnation or a terminal state comes from a complete
// process record. Observation errors never authorize release of memory still
// protected by its reader slot.
inline process_identity_result_t probe_process_identity_native(const process_incarnation_t& owner)
{
    const uint32_t pid = owner.pid;
    const uint64_t start_stamp = owner.start_stamp;
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
    linux_process_stat_t record;
    const int error = read_linux_process_stat(path.c_str(), record);
    if (error == ENOENT || error == ESRCH) {
        return confirm_linux_process_absence(static_cast<pid_t>(pid), error);
    }
    if (error != 0) {
        return unknown_process_identity(error);
    }

    // A zombie leader whose other threads still run is live (a1c835e1).
    const bool incarnation_ended = record.start_stamp != start_stamp ||
        ((record.state == 'Z' || record.state == 'X') && record.num_threads == 1);
    if (!incarnation_ended) {
        return {Process_identity_status::LIVE, {}};
    }
    if (linux_record_coordinates_contradict(owner.namespaces)) {
        return unknown_process_identity(k_foreign_process_record_error);
    }
    return {Process_identity_status::DEAD, {}};
#elif defined(__APPLE__)
    // Argument 1 includes zombies, whose records carry their terminal status.
    struct proc_bsdinfo record{};
    errno = 0;
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const int size = process_identity_proc_pidinfo(
        static_cast<int>(pid), PROC_PIDTBSDINFO, 1, &record, sizeof(record));
#else
    const int size = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 1, &record, sizeof(record));
#endif
    if (size <= 0) {
        const int error = errno ? errno : EIO;
        if (error == ESRCH) {
            return confirm_absence_by_signal(static_cast<pid_t>(pid), error);
        }
        return unknown_process_identity(error);
    }
    if (static_cast<size_t>(size) != sizeof(record)) {
        return unknown_process_identity(EIO);
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
    freebsd_process_record_t observed;
    const int error = read_freebsd_process_record(pid, observed);
    if (error != 0) {
        if (error == ESRCH) {
            return confirm_absence_by_signal(static_cast<pid_t>(pid), error);
        }
        return unknown_process_identity(error);
    }
    // A successful empty result is a missing record; a short one is malformed.
    if (observed.size == 0) {
        return confirm_absence_by_signal(static_cast<pid_t>(pid), ESRCH);
    }
    if (observed.size != sizeof(observed.record)) {
        return unknown_process_identity(EIO);
    }
    // A terminal record ends the published incarnation, whichever one it shows.
    if (observed.record.ki_stat == SZOMB
#ifdef SDEAD
        || observed.record.ki_stat == SDEAD
#endif
    ) {
        return {Process_identity_status::DEAD, {}};
    }
    if (observed.stamp_error != 0) {
        return unknown_process_identity(observed.stamp_error);
    }
    if (observed.start_stamp != start_stamp) {
        return {Process_identity_status::DEAD, {}};
    }
    return {Process_identity_status::LIVE, {}};
#else
    return unknown_process_identity(ENOTSUP);
#endif
#endif
}

} // namespace detail

inline process_identity_result_t probe_process_identity(const process_incarnation_t& owner)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (detail::process_identity_probe_hook) {
        return detail::process_identity_probe_hook(owner);
    }
#endif
    return detail::probe_process_identity_native(owner);
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

// Names the encoding of the marker's start stamp. FreeBSD's uptime-based stamp
// has its own key, so a build that records the clock-dependent ki_start under
// start_ns and this build never compare the two: each judges the other's
// markers by process liveness alone.
inline const char* run_marker_start_stamp_key()
{
#if defined(__FreeBSD__)
    return "start_uptime_ns";
#else
    return "start_ns";
#endif
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

    std::ostringstream marker;
    marker << "pid=" << record.pid << '\n';
    marker << run_marker_start_stamp_key() << '=' << record.start_stamp << '\n';
    marker << "created_ns=" << record.created_monotonic_ns << '\n';
    marker << "occurrence=" << record.recovery_occurrence << '\n';
    return detail::write_private_file(run_marker_path(directory), marker.str());
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
            if (key == "pid")                        { record.pid = static_cast<uint32_t>(std::stoul(value)); } else
            if (key == run_marker_start_stamp_key()) { record.start_stamp = std::stoull(value);               } else
            if (key == "created_ns")                 { record.created_monotonic_ns = std::stoull(value);      } else
            if (key == "occurrence")                 { record.recovery_occurrence = static_cast<uint32_t>(std::stoul(value)); }
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

    std::ostringstream cleanup_file;
    if (record_opt) {
        const auto& record = *record_opt;
        cleanup_file << "pid=" << record.pid << '\n';
        cleanup_file << run_marker_start_stamp_key() << '=' << record.start_stamp << '\n';
        cleanup_file << "created_ns=" << record.created_monotonic_ns << '\n';
        cleanup_file << "occurrence=" << record.recovery_occurrence << '\n';
    }
    if (!detail::write_private_file(cleanup, cleanup_file.str())) {
        return;
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
    if (!detail::private_directory_owned(base_dir)) {
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
        if (!detail::private_directory_owned(it->path())) {
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

        (void)detail::remove_private_directory_tree(dir_path);
    }
}

} // namespace sintra
