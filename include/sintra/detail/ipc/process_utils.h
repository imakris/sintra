// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "observation.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../time_utils.h"
#include "platform_defs.h"
#include "file_utils.h"
#include "private_directory_lease.h"

#ifdef _WIN32
  #include "../sintra_windows.h"
#else
  #include <cerrno>
  #include <dirent.h>
  #include <fcntl.h>
  #include <pthread.h>
  #include <signal.h>
  #include <sys/stat.h>
  #include <sys/file.h>
  #include <sys/types.h>
  #include <unistd.h>

  #if defined(__linux__)
    #include <sys/file.h>
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
// same estimate, and the stamp is ki_start minus a boot time that reads the
// same before and after the record. No snapshot covers the three lookups, so
// that stability is best-effort evidence, never proof: a change reversed
// between the two boot-time reads goes unseen, and the stamp of a live
// process is then wrong, at capture as at observation. A different stamp in a
// nonterminal record therefore proves no other incarnation
// (probe_process_identity_native). A boot time that moves during every
// attempt leaves the stamp unavailable (EAGAIN).
inline constexpr int k_freebsd_boot_time_attempts = 3;

// The error of an UNKNOWN result whose nonterminal record shows a start stamp
// other than the published one.
inline constexpr int k_start_stamp_mismatch_error = ESTALE;

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

#ifdef _WIN32
namespace detail {

enum class Process_liveness { DEAD, LIVE, UNKNOWN };

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline decltype(&::OpenProcess) process_identity_open_process = ::OpenProcess;
inline decltype(&::WaitForSingleObject) process_identity_wait_for_single_object = ::WaitForSingleObject;
#endif

// A waitable handle distinguishes a running process from a signaled one even
// when its exit code is STILL_ACTIVE (259). Lookup and wait errors are not
// evidence that the PID has exited.
inline Process_liveness probe_process_liveness(uint32_t pid) noexcept try
{
    if (pid == 0) {
        return Process_liveness::DEAD;
    }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    HANDLE process = process_identity_open_process(SYNCHRONIZE, FALSE, pid);
#else
    HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
#endif
    if (!process) {
        return ::GetLastError() == ERROR_INVALID_PARAMETER
            ? Process_liveness::DEAD : Process_liveness::UNKNOWN;
    }
    struct Process_handle
    {
        HANDLE handle;
        ~Process_handle() noexcept { ::CloseHandle(handle); }
    } retained{process};
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const DWORD observed = process_identity_wait_for_single_object(process, 0);
#else
    const DWORD observed = ::WaitForSingleObject(process, 0);
#endif
    if (observed == WAIT_OBJECT_0) {
        return Process_liveness::DEAD;
    }
    if (observed == WAIT_TIMEOUT) {
        return Process_liveness::LIVE;
    }
    return Process_liveness::UNKNOWN;
}
catch (...) {
    defer_observation_failure("process_liveness");
    return Process_liveness::UNKNOWN;
}

} // namespace detail
#endif

inline bool is_process_alive(uint32_t pid) noexcept try
{
#ifdef _WIN32
    // Public bool callers may use this for cleanup, where an unproven death
    // must preserve the process's resources.
    return detail::probe_process_liveness(pid) != detail::Process_liveness::DEAD;
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
catch (...) {
    detail::defer_observation_failure("process_liveness");
    return true;
}

namespace detail {

// A process instance identifies one process image: its PID in the upper 32
// bits and, in the lower 32, a random token that the image, or each fork
// child, draws when it first needs one. Shared locks record their owner as a
// process instance. Two live processes never share a PID, so an owner
// recorded with this process's PID and another token was recorded by an
// earlier process with this PID, which has exited or replaced its image. A
// token collision hides that and fails safe, as ownership by this process.
// Like Sintra's runtime state, the token belongs to the one copy of Sintra in
// the process.
inline constexpr uint32_t process_instance_pid(uint64_t instance) noexcept
{
    return static_cast<uint32_t>(instance >> 32);
}

// Mixes platform randomness with clocks and an address, which still differ
// between process instances where no random device is available.
inline uint32_t draw_process_instance_token() noexcept
{
    uint64_t entropy =
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) *
            0x9e3779b97f4a7c15ull;
    entropy ^= static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(&entropy));
    try {
        std::random_device device;
        entropy ^= (static_cast<uint64_t>(device()) << 32) | device();
    }
    catch (...) {
    }
    // splitmix64 finaliser
    entropy = (entropy ^ (entropy >> 30)) * 0xbf58476d1ce4e5b9ull;
    entropy = (entropy ^ (entropy >> 27)) * 0x94d049bb133111ebull;
    entropy ^= entropy >> 31;
    return static_cast<uint32_t>(entropy ^ (entropy >> 32));
}

// This process's instance, or zero before its first use.
inline std::atomic<uint64_t>& cached_process_instance() noexcept
{
    static std::atomic<uint64_t> s_instance{0};
    return s_instance;
}

#ifndef _WIN32
#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace test_hooks {
inline std::atomic<int> s_process_instance_fork_error{0};
}
#endif
// A fork child copies its parent's instance, and so would the fork children
// of a child that never used it; one of those can receive the parent's PID
// after the parent exits. Every fork child therefore starts without one.
inline void forget_process_instance_in_fork_child() noexcept
{
    cached_process_instance().store(0, std::memory_order_relaxed);
}

inline bool register_process_instance_fork_handler()
{
    int injected = 0;
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    injected = test_hooks::s_process_instance_fork_error.load();
#endif
    const int error = injected ? injected :
        ::pthread_atfork(nullptr, nullptr, forget_process_instance_in_fork_child);
    if (error != 0) {
        throw std::system_error(error, std::system_category(), "pthread_atfork");
    }
    return true;
}
#endif

inline uint64_t current_process_instance()
{
#ifndef _WIN32
    // Concurrent first callers wait until the fork handler is registered, so
    // it precedes the first published instance. A failed registration throws,
    // publishes nothing, and is attempted again by the next call.
    [[maybe_unused]] static const bool s_fork_handler_registered =
        register_process_instance_fork_handler();
#endif
    auto& cached = cached_process_instance();
    const uint32_t pid = get_current_pid();
    uint64_t instance = cached.load(std::memory_order_acquire);
    // _Fork and raw process creation can bypass pthread_atfork. A different
    // PID requires a fresh instance, even when the inherited cache is nonzero.
    // This cannot detect an unused descendant receiving the cached ancestor PID.
    while (instance == 0 || process_instance_pid(instance) != pid) {
        const uint64_t drawn =
            (static_cast<uint64_t>(pid) << 32) | draw_process_instance_token();
        if (cached.compare_exchange_strong(
                instance, drawn, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return drawn;
        }
    }
    return instance;
}

// Whether the process instance that recorded a shared lock's owner has
// exited, as this process instance observes it.
inline bool process_instance_has_exited(uint64_t recorded, uint64_t self)
{
    const uint32_t pid = process_instance_pid(recorded);
    if (pid == process_instance_pid(self)) {
        return recorded != self;
    }
#ifdef _WIN32
    return probe_process_liveness(pid) == Process_liveness::DEAD;
#else
    return !is_process_alive(pid);
#endif
}

} // namespace detail

inline std::optional<uint64_t> query_process_start_stamp(uint32_t pid) noexcept try
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
catch (...) {
    detail::defer_observation_failure("process_start_stamp");
    return std::nullopt;
}

// Whether the start stamp observed for a live PID proves that the PID no longer
// names the incarnation that published published_stamp. On FreeBSD it never
// does: a missed boot-time change can make the stamps of one process differ
// (read_freebsd_process_record). Recovery keyed by such a PID then waits until
// the process holding it exits.
inline bool start_stamp_proves_other_incarnation(uint64_t published_stamp, uint64_t observed_stamp)
{
#if defined(__FreeBSD__)
    (void)published_stamp;
    (void)observed_stamp;
    return false;
#else
    return observed_stamp != published_stamp;
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
inline decltype(&::GetProcessTimes) process_identity_get_process_times = ::GetProcessTimes;
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
    struct Status_file
    {
        int fd;
        ~Status_file() noexcept { ::close(fd); }
    } retained{fd};
    std::string status;
    char buffer[4096];
    ssize_t size = 0;
    while ((size = ::read(fd, buffer, sizeof(buffer))) > 0) {
        status.append(buffer, static_cast<size_t>(size));
    }

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

inline std::optional<uint64_t> current_process_start_stamp() noexcept try
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
catch (...) {
    detail::defer_observation_failure("current_process_start_stamp");
    return std::nullopt;
}

// Captures this process's incarnation for one reader-slot acquisition. On
// Linux its namespaces are read afresh, alongside the start stamp.
inline std::optional<process_incarnation_t> current_process_incarnation() noexcept try
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
catch (...) {
    detail::defer_observation_failure("current_process_incarnation");
    return std::nullopt;
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
    struct Process_handle
    {
        HANDLE handle;
        ~Process_handle() noexcept { ::CloseHandle(handle); }
    } retained{process};

    FILETIME creation{}, exit{}, kernel{}, user{};
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const BOOL queried = process_identity_get_process_times(process, &creation, &exit, &kernel, &user);
#else
    const BOOL queried = ::GetProcessTimes(process, &creation, &exit, &kernel, &user);
#endif
    if (!queried) {
        const DWORD error = ::GetLastError();
        return unknown_process_identity(error);
    }

    ULARGE_INTEGER observed_stamp{};
    observed_stamp.LowPart = creation.dwLowDateTime;
    observed_stamp.HighPart = creation.dwHighDateTime;
    if (observed_stamp.QuadPart != start_stamp) {
        return {Process_identity_status::DEAD, {}};
    }

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    const DWORD wait_result = process_identity_wait_for_single_object(process, 0);
#else
    const DWORD wait_result = ::WaitForSingleObject(process, 0);
#endif
    const DWORD error = wait_result == WAIT_FAILED ? ::GetLastError() : ERROR_INVALID_DATA;
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
    // A missed boot-time change can make the stamps differ for one process.
    // If the published process died and a live one took its PID before this
    // lookup, the owner therefore stays UNKNOWN until that process exits.
    if (observed.start_stamp != start_stamp) {
        return unknown_process_identity(k_start_stamp_mismatch_error);
    }
    return {Process_identity_status::LIVE, {}};
#else
    return unknown_process_identity(ENOTSUP);
#endif
#endif
}

} // namespace detail

inline process_identity_result_t probe_process_identity(const process_incarnation_t& owner) noexcept try
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (detail::process_identity_probe_hook) {
        return detail::process_identity_probe_hook(owner);
    }
#endif
    return detail::probe_process_identity_native(owner);
}
catch (...) {
    detail::defer_observation_failure("process_identity");
    return detail::unknown_process_identity(EIO);
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
    // Published in one step: a scan that found a partly written marker would
    // take it for a malformed one and delete a live swarm's directory.
    return detail::publish_private_file(run_marker_path(directory), marker.str());
}

inline std::optional<run_marker_record_t> read_run_marker(
    const std::filesystem::path& marker_path,
    bool*                        read_succeeded = nullptr)
{
    if (read_succeeded) {
        *read_succeeded = false;
    }

    std::string contents;
#ifdef _WIN32
    const HANDLE file = ::CreateFileW(marker_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    char buffer[4096];
    DWORD bytes_read = 0;
    bool complete = true;
    while (true) {
        if (!::ReadFile(file, buffer, sizeof(buffer), &bytes_read, nullptr)) {
            complete = false;
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        contents.append(buffer, bytes_read);
    }
    const bool closed = ::CloseHandle(file) != 0;
    if (!complete || !closed) {
        return std::nullopt;
    }
#else
    std::ifstream file(marker_path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer))) {
        contents.append(buffer, sizeof(buffer));
    }
    contents.append(buffer, static_cast<std::size_t>(file.gcount()));
    if (!file.eof() || file.bad()) {
        return std::nullopt;
    }
#endif
    if (read_succeeded) {
        *read_succeeded = true;
    }
    std::istringstream marker(contents);

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

#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace detail::test_hooks {
// Runs when a scan of swarm directories begins, before it reads any marker.
inline void (*s_swarm_directory_scan_started)(const std::filesystem::path& base_dir) = nullptr;
}
#endif

inline void cleanup_stale_swarm_directories(
    const std::filesystem::path&   base_dir,
    uint32_t                       current_pid,
    uint64_t                       current_start_stamp,
    bool                           direct = false)
{
#ifndef _WIN32
    // Per-account roots are no longer used on POSIX. Their markers have no
    // cleanup domain, so a PID lookup cannot safely classify them.
    (void)base_dir;
    (void)current_pid;
    (void)current_start_stamp;
    (void)direct;
    return;
#else
    std::error_code ec;
    if (!direct && !detail::private_directory_owned(base_dir)) {
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

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (detail::test_hooks::s_swarm_directory_scan_started) {
        detail::test_hooks::s_swarm_directory_scan_started(base_dir);
    }
#endif

    std::vector<std::filesystem::path> directories;
    const auto prefix = detail::private_swarm_root().filename().string() + '-';
    for (std::filesystem::directory_iterator it(base_dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        const auto name = it->path().filename().string();
        if (direct && (name.size() != prefix.size() + 16 ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos))
        {
            continue;
        }
        directories.push_back(it->path());
    }
    if (ec) { return; }
    for (const auto& dir_path : directories) {
        detail::Private_directory_removal removal(dir_path);
        if (!removal.valid()) {
            continue;
        }

        const auto  marker_path  = run_marker_path(dir_path);
        const auto  cleanup_path = run_marker_cleanup_path(dir_path);

        std::error_code exists_ec;
        const bool has_marker = std::filesystem::exists(marker_path, exists_ec);
        exists_ec.clear();
        const bool has_cleanup = std::filesystem::exists(cleanup_path, exists_ec);

        if (!has_marker && !has_cleanup) {
            continue;
        }

        bool read_succeeded = false;
        auto record_opt = read_run_marker(
            has_marker ? marker_path : cleanup_path, &read_succeeded);
        if (!read_succeeded) {
            continue;
        }
        // Read after the marker. The monotonic clock is system-wide and
        // restarts at boot, so a marker created later than this was created
        // before a reboot, whatever process now holds its PID.
        const auto now_monotonic = monotonic_now_ns();
        bool stale = has_cleanup;

        if (!record_opt) {
            stale = true;
        }
        else {
            const auto& record = *record_opt;
            if (record.pid == current_pid) {
                if (record.start_stamp != 0 && current_start_stamp != 0 &&
                    start_stamp_proves_other_incarnation(record.start_stamp, current_start_stamp))
                {
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
                    if (running_start &&
                        start_stamp_proves_other_incarnation(record.start_stamp, *running_start))
                    {
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

        (void)removal.remove();
    }
#endif
}

#ifndef _WIN32
namespace detail {

struct Cleanup_domain
{
    std::string         boot_id;
    process_namespace_t pid;
    process_namespace_t time;
};

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline std::optional<Cleanup_domain> (*private_cleanup_domain_for_test)() = nullptr;
inline void (*before_private_swarm_removal_for_test)(const std::filesystem::path&) = nullptr;
inline void (*before_owned_swarm_lock_for_test)(const std::filesystem::path&) = nullptr;
#endif

inline std::optional<Cleanup_domain> current_private_cleanup_domain()
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (private_cleanup_domain_for_test) {
        return private_cleanup_domain_for_test();
    }
#endif
#if defined(__linux__)
    std::ifstream boot_file("/proc/sys/kernel/random/boot_id");
    std::string boot_id;
    if (!std::getline(boot_file, boot_id) || boot_id.size() != 36) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < boot_id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (boot_id[i] != '-') {
                return std::nullopt;
            }
        }
        else if (!std::isxdigit(static_cast<unsigned char>(boot_id[i]))) {
            return std::nullopt;
        }
    }
    const auto read_namespace = [](const char* path) {
        struct stat status{};
        // ENOENT also occurs when procfs is masked. It never proves that the
        // kernel lacks this namespace feature.
        return ::stat(path, &status) == 0 && S_ISREG(status.st_mode)
            ? process_namespace_t{Process_metadata_state::VALID,
                  static_cast<uint64_t>(status.st_dev), static_cast<uint64_t>(status.st_ino)}
            : process_namespace_t{};
    };
    Cleanup_domain domain{boot_id,
        read_namespace("/proc/self/ns/pid"), read_namespace("/proc/self/ns/time")};
    if (domain.pid.state == Process_metadata_state::UNKNOWN ||
        domain.time.state == Process_metadata_state::UNKNOWN)
    {
        return std::nullopt;
    }
    return domain;
#else
    return std::nullopt;
#endif
}

inline std::string private_cleanup_domain_contents(const Cleanup_domain& domain)
{
    auto namespace_value = [](const process_namespace_t& value) {
        if (value.state == Process_metadata_state::ABSENT) {
            return std::string("absent");
        }
        return std::to_string(value.device) + ':' + std::to_string(value.inode);
    };
    return "sintra-cleanup-domain=1\nboot=" + domain.boot_id + "\npid=" +
        namespace_value(domain.pid) + "\ntime=" + namespace_value(domain.time) + '\n';
}

inline std::optional<Cleanup_domain> parse_private_cleanup_domain(const std::string& contents)
{
    std::istringstream input(contents);
    std::string version, boot, pid, time, trailing;
        if (!std::getline(input, version) || version != "sintra-cleanup-domain=1" ||
        !std::getline(input, boot) || boot.rfind("boot=", 0) != 0 ||
        !std::getline(input, pid) || pid.rfind("pid=", 0) != 0 ||
        !std::getline(input, time) || time.rfind("time=", 0) != 0 ||
        std::getline(input, trailing))
    {
        return std::nullopt;
    }
    const auto parse_namespace = [](const std::string& text) -> std::optional<process_namespace_t> {
        if (text == "absent") {
            return process_namespace_t{Process_metadata_state::ABSENT, 0, 0};
        }
        const auto separator = text.find(':');
        if (separator == std::string::npos || text.find(':', separator + 1) != std::string::npos) {
            return std::nullopt;
        }
        try {
            std::size_t first = 0, second = 0;
            const auto device = std::stoull(text.substr(0, separator), &first);
            const auto inode = std::stoull(text.substr(separator + 1), &second);
            if (first != separator || second != text.size() - separator - 1 ||
                device == 0 || inode == 0)
            {
                return std::nullopt;
            }
            if (std::to_string(device) + ':' + std::to_string(inode) != text) {
                return std::nullopt;
            }
            return process_namespace_t{Process_metadata_state::VALID, device, inode};
        }
        catch (...) {
            return std::nullopt;
        }
    };
    const auto parsed_pid = parse_namespace(pid.substr(4));
    const auto parsed_time = parse_namespace(time.substr(5));
    if (!parsed_pid || !parsed_time || boot.size() != 41) {
        return std::nullopt;
    }
    const auto boot_id = boot.substr(5);
    for (std::size_t i = 0; i < boot_id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (boot_id[i] != '-') {
                return std::nullopt;
            }
        }
        else if (!std::isxdigit(static_cast<unsigned char>(boot_id[i]))) {
            return std::nullopt;
        }
    }
    return Cleanup_domain{boot_id, *parsed_pid, *parsed_time};
}

inline bool same_private_cleanup_domain(const Cleanup_domain& a, const Cleanup_domain& b)
{
    return a.boot_id == b.boot_id &&
        a.pid.state == b.pid.state && a.pid.device == b.pid.device && a.pid.inode == b.pid.inode &&
        a.time.state == b.time.state && a.time.device == b.time.device && a.time.inode == b.time.inode;
}

inline constexpr const char* private_cleanup_domain_filename() { return "sintra_cleanup_domain"; }

inline void publish_private_cleanup_domain(const std::filesystem::path& directory)
{
    if (const auto domain = current_private_cleanup_domain()) {
        (void)publish_private_file(directory / private_cleanup_domain_filename(),
            private_cleanup_domain_contents(*domain));
    }
}

inline bool private_directory_handle_owned(int fd)
{
    struct stat status{};
    return ::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode) &&
        status.st_uid == geteuid() && (status.st_mode & 0777) == 0700;
}

inline bool read_private_file_at(int directory, const char* name, std::string& contents)
{
    const int fd = ::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    if (!private_file_owned(fd)) {
        ::close(fd);
        return false;
    }
    char buffer[4096];
    std::size_t size = 0;
    while (size < sizeof(buffer)) {
        const auto read_count = ::read(fd, buffer + size, sizeof(buffer) - size);
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count < 0) {
            ::close(fd);
            return false;
        }
        if (read_count == 0) {
            break;
        }
        size += static_cast<std::size_t>(read_count);
    }
    char extra = 0;
    const bool overflow = size == sizeof(buffer) && ::read(fd, &extra, 1) != 0;
    ::close(fd);
    if (overflow) {
        return false;
    }
    contents.assign(buffer, size);
    return true;
}

inline bool private_swarm_name(const std::string& name, bool lease = false)
{
    const auto prefix = std::string(lease ? "sintra-l1-" : "sintra-") + std::to_string(geteuid()) + '-';
    if (name.size() != prefix.size() + 16 || name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    bool nonzero = false;
    for (std::size_t i = prefix.size(); i < name.size(); ++i) {
        const char digit = name[i];
        if (!(digit >= '0' && digit <= '9') && !(digit >= 'a' && digit <= 'f')) {
            return false;
        }
        nonzero |= digit != '0';
    }
    return nonzero;
}

inline bool remove_private_directory_tree_at(
    int parent_fd, const char* name, unsigned depth = 0, int expected_fd = -1)
{
    if (depth > 4) {
        return false;
    }
    const int fd = ::openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    struct stat directory_status{};
    if (!private_directory_handle_owned(fd) || ::fstat(fd, &directory_status) != 0) {
        ::close(fd);
        return false;
    }
    if (expected_fd >= 0) {
        struct stat expected_status{};
        if (::fstat(expected_fd, &expected_status) != 0 ||
            expected_status.st_dev != directory_status.st_dev ||
            expected_status.st_ino != directory_status.st_ino)
        {
            ::close(fd);
            return false;
        }
    }
    DIR* entries = ::fdopendir(fd);
    if (!entries) {
        ::close(fd);
        return false;
    }
    bool safe = true;
    std::vector<std::string> children;
    while (safe) {
        errno = 0;
        const auto* entry = ::readdir(entries);
        if (!entry) {
            safe = errno == 0;
            break;
        }
        const std::string child = entry->d_name;
        if (child == "." || child == "..") {
            continue;
        }
        children.push_back(child);
    }
    for (const auto& child : children) {
        if (!safe) {
            break;
        }
        struct stat status{};
        if (::fstatat(fd, child.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            safe = false;
        }
        else
        if (S_ISDIR(status.st_mode)) {
            safe = remove_private_directory_tree_at(fd, child.c_str(), depth + 1);
        }
        else
        if (S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
            (status.st_mode & 0077) == 0)
        {
            safe = ::unlinkat(fd, child.c_str(), 0) == 0;
        }
        else {
            safe = false;
        }
    }
    ::closedir(entries);
    if (!safe) {
        return false;
    }
    struct stat named_status{};
    return ::fstatat(parent_fd, name, &named_status, AT_SYMLINK_NOFOLLOW) == 0 &&
        named_status.st_dev == directory_status.st_dev &&
        named_status.st_ino == directory_status.st_ino &&
        ::unlinkat(parent_fd, name, AT_REMOVEDIR) == 0;
}

} // namespace detail

inline void cleanup_stale_private_swarms(const std::filesystem::path& temp_directory)
{
    const auto current_domain = detail::current_private_cleanup_domain();
    const int temp_fd = ::open(temp_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (temp_fd < 0) {
        return;
    }
    struct stat temp_status{};
    if (::fstat(temp_fd, &temp_status) != 0 ||
        (temp_status.st_uid != 0 && temp_status.st_uid != geteuid()) ||
        ((temp_status.st_mode & 0022) && !(temp_status.st_mode & 01000)))
    {
        ::close(temp_fd);
        return;
    }
    const int scan_fd = ::dup(temp_fd);
    DIR* entries = scan_fd < 0 ? nullptr : ::fdopendir(scan_fd);
    if (!entries) {
        if (scan_fd >= 0) {
            ::close(scan_fd);
        }
        ::close(temp_fd);
        return;
    }
    std::vector<std::string> names;
    bool complete = true;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(entries);
        if (!entry) {
            complete = errno == 0;
            break;
        }
        names.emplace_back(entry->d_name);
    }
    ::closedir(entries);
    if (!complete) { ::close(temp_fd); return; }
    for (const auto& name : names) {
        const bool lease = detail::private_swarm_name(name, true);
        if (!lease && (!current_domain || !detail::private_swarm_name(name))) {
            continue;
        }
        const int swarm_fd = ::openat(temp_fd, name.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (swarm_fd < 0) {
            continue;
        }
        if (!detail::private_directory_handle_owned(swarm_fd)) {
            ::close(swarm_fd);
            continue;
        }
        // A normal coordinator teardown takes this same inode lock. Keeping
        // it through rmdir prevents a removed name from being reused between
        // the final identity check and unlinkat.
        if (lease && ::flock(swarm_fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(swarm_fd);
            continue;
        }
        const auto path = temp_directory / name;
        if (lease) {
#if defined(SINTRA_ENABLE_TEST_HOOKS)
            if (detail::before_private_swarm_removal_for_test) {
                detail::before_private_swarm_removal_for_test(path);
            }
#endif
            (void)detail::remove_private_directory_tree_at(temp_fd, name.c_str(), 0, swarm_fd);
            ::close(swarm_fd);
            continue;
        }
#if defined(__linux__)
        if (::flock(swarm_fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(swarm_fd);
            continue;
        }
#endif
        std::string contents;
        const bool read_domain = detail::read_private_file_at(
            swarm_fd, detail::private_cleanup_domain_filename(), contents);
        const auto stored_domain = read_domain ? detail::parse_private_cleanup_domain(contents) : std::nullopt;
        if (!stored_domain || !detail::same_private_cleanup_domain(*stored_domain, *current_domain)) {
            ::close(swarm_fd);
            continue;
        }
        // The directory is owner-only, and a writable temp parent is sticky.
        // Other accounts cannot replace this entry between the handle check
        // and the marker read; same-account processes share Sintra's trust.
        const auto marker = run_marker_path(path);
        const auto cleanup = run_marker_cleanup_path(path);
        if (!detail::private_file_path_owned(marker) && !detail::private_file_path_owned(cleanup)) {
            ::close(swarm_fd);
            continue;
        }
        bool read_succeeded = false;
        const bool has_cleanup = detail::private_file_path_owned(cleanup);
        const auto record = read_run_marker(has_cleanup ? cleanup : marker, &read_succeeded);
        if (!read_succeeded) {
            ::close(swarm_fd);
            continue;
        }
        bool stale = has_cleanup || !record;
        if (record && !stale) {
            // A native absence lookup uses the cleaner's PID namespace. A
            // procfs record may be mounted for an ancestor namespace, so it
            // cannot establish a different incarnation for this cleanup.
            if (static_cast<uint64_t>(record->pid) <=
                static_cast<uint64_t>(std::numeric_limits<pid_t>::max()))
            {
                stale = ::kill(static_cast<pid_t>(record->pid), 0) != 0 && errno == ESRCH;
            }
        }
        if (stale) {
#if defined(SINTRA_ENABLE_TEST_HOOKS)
            if (detail::before_private_swarm_removal_for_test) {
                detail::before_private_swarm_removal_for_test(path);
            }
#endif
            (void)detail::remove_private_directory_tree_at(temp_fd, name.c_str(), 0, swarm_fd);
        }
        ::close(swarm_fd);
    }
    ::close(temp_fd);
}
#endif

inline void cleanup_owned_swarm_directory(const std::filesystem::path& directory,
    int retained_lease = -1, bool locks_unavailable = false,
    const detail::Private_directory_identity* expected = nullptr)
{
#ifndef _WIN32
    if (locks_unavailable) {
        const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { return; }
        struct stat status{};
        const bool matches = expected && expected->valid && ::fstat(fd, &status) == 0 &&
            expected->device == static_cast<std::uint64_t>(status.st_dev) &&
            expected->inode == static_cast<std::uint64_t>(status.st_ino);
        if (matches) {
            const int parent = ::open(directory.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (parent >= 0) {
                (void)detail::remove_private_directory_tree_at(parent, directory.filename().c_str(), 0, fd);
                ::close(parent);
            }
        }
        ::close(fd);
        return;
    }
    if (retained_lease >= 0) {
        const int parent = ::open(directory.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) {
            (void)detail::remove_private_directory_tree_at(parent,
                directory.filename().c_str(), 0, retained_lease);
            ::close(parent);
        }
        return;
    }
#else
    (void)retained_lease;
    (void)locks_unavailable;
    detail::Private_directory_removal removal(directory, expected);
    if (removal.valid()) { (void)removal.remove(); }
    return;
#endif
#if defined(__linux__)
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return;
    }
    if (!detail::private_directory_handle_owned(fd)) {
        ::close(fd);
        return;
    }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (detail::before_owned_swarm_lock_for_test) {
        detail::before_owned_swarm_lock_for_test(directory);
    }
#endif
    int lock_result = 0;
    do {
        lock_result = ::flock(fd, LOCK_EX);
    }
    while (lock_result != 0 && errno == EINTR);
    struct stat opened_status{};
    struct stat named_status{};
    const bool same_directory = lock_result == 0 &&
        ::fstat(fd, &opened_status) == 0 &&
        ::lstat(directory.c_str(), &named_status) == 0 &&
        opened_status.st_dev == named_status.st_dev &&
        opened_status.st_ino == named_status.st_ino;
    if (same_directory) {
        mark_run_directory_for_cleanup(directory);
        (void)detail::remove_private_directory_tree(directory);
    }
    ::close(fd);
#else
    mark_run_directory_for_cleanup(directory);
    (void)detail::remove_private_directory_tree(directory);
#endif
}

} // namespace sintra
