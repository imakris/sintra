// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// Deterministic replacements for the native observations behind
// sintra::probe_process_identity. Every fake forwards to the native call
// until a test configures it; Scoped_fakes restores the native calls.

#pragma once

#include <sintra/detail/ipc/process_utils.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace sintra::test::identity_fakes {

#ifndef _WIN32
inline constexpr int k_native = -1;
inline constexpr int k_succeed = 0;

// Result of kill(pid, 0): k_native, k_succeed or an errno.
inline int s_kill_result = k_native;

inline int fake_kill(pid_t pid, int signal)
{
    if (s_kill_result == k_native) {
        return ::kill(pid, signal);
    }
    if (s_kill_result == k_succeed) {
        return 0;
    }
    errno = s_kill_result;
    return -1;
}
#endif

#if defined(__linux__)
// Opening /proc/<pid>/stat fails with ENOENT for every numeric PID.
inline bool s_hide_process_records = false;
// Result of pidfd_open: k_native, k_succeed or an errno.
inline int s_pidfd_result = k_native;
// Namespace entries of this process; empty keeps the native entries. VALID
// entries report their device and inode, ABSENT fails with ENOENT and
// UNKNOWN with EACCES.
inline std::optional<process_namespaces_t> s_namespaces;
// Opening /proc/self/status fails with this errno, or opens this replacement.
inline int s_status_error = 0;
inline std::filesystem::path s_status_path;

inline bool is_process_record_path(const char* path)
{
    const std::string text(path);
    const std::string prefix = "/proc/";
    const std::string suffix = "/stat";
    if (text.size() <= prefix.size() + suffix.size() || text.compare(0, prefix.size(), prefix) != 0 ||
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) != 0)
    {
        return false;
    }
    const auto digits = text.substr(prefix.size(), text.size() - prefix.size() - suffix.size());
    return digits.find_first_not_of("0123456789") == std::string::npos;
}

inline int fake_open(const char* path, int flags, ...)
{
    if (s_hide_process_records && is_process_record_path(path)) {
        errno = ENOENT;
        return -1;
    }
    if (std::strcmp(path, "/proc/self/status") == 0) {
        if (s_status_error != 0) {
            errno = s_status_error;
            return -1;
        }
        if (!s_status_path.empty()) {
            return ::open(s_status_path.c_str(), flags);
        }
    }
    return ::open(path, flags);
}

inline int fake_stat(const char* path, struct stat* status)
{
    if (!s_namespaces) {
        return ::stat(path, status);
    }
    const bool pid_entry = std::strcmp(path, "/proc/self/ns/pid") == 0;
    const auto& entry = pid_entry ? s_namespaces->pid : s_namespaces->time;
    switch (entry.state) {
        case Process_metadata_state::VALID:
            *status = {};
            status->st_dev = static_cast<dev_t>(entry.device);
            status->st_ino = static_cast<ino_t>(entry.inode);
            return 0;
        case Process_metadata_state::ABSENT:
            errno = ENOENT;
            return -1;
        case Process_metadata_state::UNKNOWN:
            break;
    }
    errno = EACCES;
    return -1;
}

inline int fake_pidfd_open(pid_t pid)
{
    if (s_pidfd_result == k_native) {
        return sintra::detail::open_process_pidfd(pid);
    }
    if (s_pidfd_result == k_succeed) {
        return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    errno = s_pidfd_result;
    return -1;
}

inline process_namespace_t valid_namespace(uint64_t inode)
{
    return {Process_metadata_state::VALID, 1, inode};
}

// Replaces /proc/self/status by a copy of it whose NStgid line holds the
// given IDs; without IDs the line is removed, as on kernels before 4.1.
inline void use_status_nstgid(const std::string& ids)
{
    std::ifstream native("/proc/self/status");
    std::ostringstream contents;
    std::string line;
    while (std::getline(native, line)) {
        if (line.compare(0, 7, "NStgid:") == 0) {
            if (!ids.empty()) {
                contents << "NStgid:\t" << ids << '\n';
            }
            continue;
        }
        contents << line << '\n';
    }
    s_status_path = std::filesystem::temp_directory_path() /
        ("sintra_identity_status_" + std::to_string(::getpid()));
    std::ofstream replacement(s_status_path, std::ios::trunc);
    replacement << contents.str();
    replacement.close();
    if (!replacement) {
        throw std::runtime_error("cannot write the replacement process status");
    }
}
#elif defined(__APPLE__) || defined(__FreeBSD__)
// Native process record, a failure with s_record_error, a successful empty
// result, or a successful short record.
enum class Record
{
    NATIVE,
    FAILED,
    EMPTY,
    SHORT
};

inline Record s_record = Record::NATIVE;
inline int s_record_error = ESRCH;
#if defined(__APPLE__)
inline uint64_t s_last_argument = ~uint64_t(0);

inline int fake_proc_pidinfo(int pid, int flavor, uint64_t argument, void* buffer, int size)
{
    s_last_argument = argument;
    switch (s_record) {
        case Record::NATIVE:
            return ::proc_pidinfo(pid, flavor, argument, buffer, size);
        case Record::FAILED:
            errno = s_record_error;
            return 0;
        case Record::EMPTY:
            return 0;
        case Record::SHORT:
            return size / 2;
    }
    return 0;
}
#else
inline int fake_sysctl(const int* name, u_int length, void* old, size_t* old_size, const void* next, size_t next_size)
{
    switch (s_record) {
        case Record::NATIVE:
            return ::sysctl(name, length, old, old_size, next, next_size);
        case Record::FAILED:
            errno = s_record_error;
            return -1;
        case Record::EMPTY:
            *old_size = 0;
            return 0;
        case Record::SHORT:
            *old_size /= 2;
            return 0;
    }
    return -1;
}
#endif
#endif

// Installs every fake, forwarding to the native calls, and restores the
// native calls and state on destruction.
class Scoped_fakes
{
public:
    Scoped_fakes()
    {
#ifndef _WIN32
        sintra::detail::process_identity_kill = fake_kill;
#endif
#if defined(__linux__)
        sintra::detail::process_identity_open_procfs = fake_open;
        sintra::detail::process_identity_stat_namespace = fake_stat;
        sintra::detail::process_identity_pidfd_open = fake_pidfd_open;
#elif defined(__APPLE__)
        sintra::detail::process_identity_proc_pidinfo = fake_proc_pidinfo;
#elif defined(__FreeBSD__)
        sintra::detail::process_identity_sysctl = fake_sysctl;
#endif
    }

    ~Scoped_fakes()
    {
#ifndef _WIN32
        sintra::detail::process_identity_kill = ::kill;
        s_kill_result = k_native;
#endif
#if defined(__linux__)
        sintra::detail::process_identity_open_procfs = ::open;
        sintra::detail::process_identity_stat_namespace = ::stat;
        sintra::detail::process_identity_pidfd_open = sintra::detail::open_process_pidfd;
        s_hide_process_records = false;
        s_pidfd_result = k_native;
        s_namespaces.reset();
        s_status_error = 0;
        if (!s_status_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(s_status_path, ignored);
            s_status_path.clear();
        }
#elif defined(__APPLE__)
        sintra::detail::process_identity_proc_pidinfo = ::proc_pidinfo;
        s_record = Record::NATIVE;
        s_record_error = ESRCH;
        s_last_argument = ~uint64_t(0);
#elif defined(__FreeBSD__)
        sintra::detail::process_identity_sysctl = ::sysctl;
        s_record = Record::NATIVE;
        s_record_error = ESRCH;
#endif
    }

    Scoped_fakes(const Scoped_fakes&) = delete;
    Scoped_fakes& operator=(const Scoped_fakes&) = delete;
};

} // namespace sintra::test::identity_fakes
