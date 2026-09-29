// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#ifndef _WIN32
#include <cerrno>
#include <filesystem>
#include <pthread.h>
#include <sys/file.h>
#include <system_error>

#include "private_resources.h"

namespace sintra::detail {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline int (*private_directory_flock_for_test)(int, int) = nullptr;
inline void (*private_lease_opened_for_test)() = nullptr;
inline void (*before_private_lease_open_for_test)(const std::filesystem::path&) = nullptr;
inline void (*before_private_lease_lock_for_test)(const std::filesystem::path&) = nullptr;
#endif

inline int lock_private_directory(int fd)
{
    int result;
    do {
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        result = private_directory_flock_for_test ?
            private_directory_flock_for_test(fd, LOCK_EX | LOCK_NB) : ::flock(fd, LOCK_EX | LOCK_NB);
#else
        result = ::flock(fd, LOCK_EX | LOCK_NB);
#endif
    } while (result != 0 && errno == EINTR);
    return result;
}

// Probe before allocating a lease-protocol name: a failed acquisition must
// never roll back by unlinking an inode another cleaner or creator has locked.
inline bool private_directory_leases_supported(const std::filesystem::path& parent)
{
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { return false; }
    const int result = lock_private_directory(fd);
    const int error = errno;
    ::close(fd);
    if (result == 0 || error == EWOULDBLOCK || error == EAGAIN) { return true; }
    return false;
}

inline bool same_private_directory(int fd, const std::filesystem::path& path)
{
    struct stat held{}, named{};
    return ::fstat(fd, &held) == 0 && ::lstat(path.c_str(), &named) == 0 &&
        S_ISDIR(named.st_mode) && held.st_dev == named.st_dev && held.st_ino == named.st_ino;
}

enum class Directory_lease_result { acquired, retry, unavailable, failed };

// A coordinator owns one open-file-description lock until directory teardown.
// Registration and descriptor changes are serialized with ordinary fork so a
// child cannot accidentally keep that ownership alive after its parent exits.
class Private_directory_lease
{
public:
    Private_directory_lease() = default;
    Private_directory_lease(const Private_directory_lease&) = delete;
    Private_directory_lease& operator=(const Private_directory_lease&) = delete;
    ~Private_directory_lease() { close(); }

    Directory_lease_result acquire(const std::filesystem::path& path,
        const Private_directory_identity* expected = nullptr)
    {
        static const bool registered = register_handlers();
        (void)registered;
        if (s_registry_pid != ::getpid()) {
            // A callback-skipping fork may inherit a locked registry mutex.
            // It must exec before constructing another coordinator.
            throw std::runtime_error("Sintra coordinator initialization after raw fork requires exec.");
        }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        if (before_private_lease_open_for_test) { before_private_lease_open_for_test(path); }
#endif
        ::pthread_mutex_lock(&s_mutex);
        m_fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (m_fd < 0) {
            ::pthread_mutex_unlock(&s_mutex);
            return Directory_lease_result::retry;
        }
        m_pid = ::getpid();
        m_next = s_first;
        s_first = this;
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        if (private_lease_opened_for_test) { private_lease_opened_for_test(); }
#endif
        ::pthread_mutex_unlock(&s_mutex);
        struct stat status{};
        if (::fstat(m_fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
            status.st_uid != ::geteuid() || (status.st_mode & 0777) != 0700)
        {
            close();
            return Directory_lease_result::failed;
        }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        if (before_private_lease_lock_for_test) { before_private_lease_lock_for_test(path); }
#endif
        const int result = lock_private_directory(m_fd);
        const int error = errno;
        struct stat opened{};
        const bool expected_inode = !expected || (::fstat(m_fd, &opened) == 0 && expected->valid &&
            expected->device == static_cast<std::uint64_t>(opened.st_dev) &&
            expected->inode == static_cast<std::uint64_t>(opened.st_ino));
        const bool same_inode = expected_inode && same_private_directory(m_fd, path);
        if (result == 0 && same_inode) { return Directory_lease_result::acquired; }
        close();
        if (!same_inode || error == EWOULDBLOCK || error == EAGAIN) {
            return Directory_lease_result::retry;
        }
        return Directory_lease_result::unavailable;
    }

    int owned_handle() const noexcept { return m_pid == ::getpid() ? m_fd : -1; }

    void close() noexcept
    {
        if (m_fd < 0) { return; }
        if (m_pid != ::getpid()) {
            // Never LOCK_UN: a fork copy refers to the parent's same lock.
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        ::pthread_mutex_lock(&s_mutex);
        auto** node = &s_first;
        while (*node && *node != this) { node = &(*node)->m_next; }
        if (*node) { *node = m_next; }
        ::close(m_fd);
        m_fd = -1;
        m_next = nullptr;
        ::pthread_mutex_unlock(&s_mutex);
    }

private:
    static bool register_handlers()
    {
        s_registry_pid = ::getpid();
        const int error = ::pthread_atfork(prepare_fork, finish_fork, child_fork);
        if (error != 0) {
            throw std::system_error(error, std::system_category(), "pthread_atfork directory leases");
        }
        return true;
    }
    static void prepare_fork()
    {
        if (s_registry_pid != ::getpid()) { s_prepared = false; return; }
        ::pthread_mutex_lock(&s_mutex);
        s_prepared = true;
    }
    static void finish_fork()
    {
        if (!s_prepared) { return; }
        s_prepared = false;
        ::pthread_mutex_unlock(&s_mutex);
    }
    static void child_fork()
    {
        if (!s_prepared) { return; }
        while (s_first) {
            auto* node = s_first;
            s_first = node->m_next;
            ::close(node->m_fd);
            node->m_fd = -1;
            node->m_next = nullptr;
        }
        s_registry_pid = ::getpid();
        s_prepared = false;
        ::pthread_mutex_unlock(&s_mutex);
    }

    int                              m_fd = -1;
    pid_t                            m_pid = 0;
    Private_directory_lease*         m_next = nullptr;
    inline static pthread_mutex_t    s_mutex = PTHREAD_MUTEX_INITIALIZER;
    inline static pid_t              s_registry_pid = 0;
    inline static bool               s_prepared = false;
    inline static Private_directory_lease* s_first = nullptr;
};

} // namespace sintra::detail
#endif
