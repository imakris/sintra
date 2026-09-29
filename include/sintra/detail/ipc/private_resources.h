// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "../../ipc/file_mapping.h"

#ifdef _WIN32
#include "../sintra_windows.h"
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace sintra::detail {

#ifdef _WIN32
class Current_user
{
public:
    Current_user(const Current_user&) = delete;
    Current_user& operator=(const Current_user&) = delete;
    Current_user()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            throw std::system_error(GetLastError(), std::system_category(), "OpenProcessToken");
        }
        DWORD needed = 0;
        const bool read = GetTokenInformation(token, TokenUser, m_user.data(),
            static_cast<DWORD>(m_user.size()), &needed) != 0;
        const auto error = GetLastError();
        CloseHandle(token);
        if (!read) {
            throw std::system_error(error, std::system_category(), "GetTokenInformation");
        }
    }

    PSID sid() const noexcept
    {
        return reinterpret_cast<const TOKEN_USER*>(m_user.data())->User.Sid;
    }

private:
    alignas(TOKEN_USER) std::array<std::byte, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> m_user{};
};

class Private_security
{
public:
    Private_security(const Private_security&) = delete;
    Private_security& operator=(const Private_security&) = delete;
    explicit Private_security(bool directory = false)
    {
        auto* acl = reinterpret_cast<ACL*>(m_acl.data());
        const DWORD inheritance = directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0;
        if (!InitializeAcl(acl, static_cast<DWORD>(m_acl.size()), ACL_REVISION) ||
            !AddAccessAllowedAceEx(acl, ACL_REVISION, inheritance, GENERIC_ALL, m_user.sid()) ||
            !InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&m_descriptor, m_user.sid(), FALSE) ||
            !SetSecurityDescriptorDacl(&m_descriptor, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&m_descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        {
            throw std::system_error(GetLastError(), std::system_category(), "Private IPC descriptor");
        }
        m_attributes = {sizeof(SECURITY_ATTRIBUTES), &m_descriptor, FALSE};
    }

    SECURITY_ATTRIBUTES* attributes() noexcept { return &m_attributes; }

private:
    Current_user m_user;
    alignas(ACL) std::array<std::byte,
        sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE> m_acl{};
    SECURITY_DESCRIPTOR m_descriptor{};
    SECURITY_ATTRIBUTES m_attributes{};
};

// The object handle needs READ_CONTROL. This uses the windows.h query rather
// than aclapi.h, whose declarations require the OPTIONAL macro that consumers
// may clean after including windows.h.
inline bool private_object_owned(HANDLE object)
{
    constexpr SECURITY_INFORMATION requested =
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    DWORD needed = 0;
    (void)GetKernelObjectSecurity(object, requested, nullptr, 0, &needed);
    if (needed == 0) {
        return false;
    }
    std::vector<std::byte> buffer(needed);
    const PSECURITY_DESCRIPTOR descriptor = buffer.data();
    Current_user user;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    BOOL owner_defaulted = FALSE;
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    bool ok = GetKernelObjectSecurity(object, requested, descriptor, needed, &needed) &&
        GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted) &&
        GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted) &&
        owner && EqualSid(owner, user.sid()) && dacl_present && dacl && dacl->AceCount != 0;
    for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
        void* entry = nullptr;
        ok = GetAce(dacl, i, &entry) != 0;
        if (ok) {
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            ok = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                EqualSid(const_cast<DWORD*>(&ace->SidStart), user.sid());
        }
    }
    return ok;
}

inline bool private_file_owned(HANDLE file)
{
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
        private_object_owned(file);
}
#else
inline bool private_file_owned(int file)
{
    struct stat info{};
    return fstat(file, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == geteuid() && (info.st_mode & 0077) == 0;
}
#endif

inline sintra::ipc::file_mapping open_private_file(
    const std::filesystem::path& path, sintra::ipc::map_mode_t mode)
{
    sintra::ipc::file_mapping file(path, mode, sintra::ipc::file_link_policy_t::reject);
    if (!private_file_owned(file.native_handle())) {
        throw std::filesystem::filesystem_error("IPC file is not private to this account",
            path, std::make_error_code(std::errc::permission_denied));
    }
    return file;
}

inline bool private_file_path_owned(const std::filesystem::path& path) noexcept
{
    try {
        (void)open_private_file(path, sintra::ipc::read_only);
        return true;
    }
    catch (...) {
        return false;
    }
}

inline bool private_directory_owned(const std::filesystem::path& directory) noexcept
{
#ifdef _WIN32
    const auto file = CreateFileW(directory.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = false;
    try {
        BY_HANDLE_FILE_INFORMATION info{};
        ok = GetFileInformationByHandle(file, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            private_object_owned(file);
    }
    catch (...) {
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return ok;
#else
    struct stat info{};
    return lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
        info.st_uid == geteuid() && (info.st_mode & 0777) == 0700;
#endif
}

struct Private_directory_identity
{
    bool valid = false;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    Private_directory_identity() = default;
    explicit Private_directory_identity(const std::filesystem::path& path) noexcept
    {
#ifdef _WIN32
        const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { return; }
        BY_HANDLE_FILE_INFORMATION info{};
        valid = GetFileInformationByHandle(handle, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        device = info.dwVolumeSerialNumber;
        inode = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        CloseHandle(handle);
#else
        struct stat info{};
        valid = ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
        device = info.st_dev;
        inode = info.st_ino;
#endif
    }

    bool matches(const Private_directory_identity& now) const noexcept
    {
        return valid && now.valid && device == now.device && inode == now.inode;
    }

    void remove_empty(const std::filesystem::path& path, bool lease_protocol = false) const noexcept
    {
        if (!matches(Private_directory_identity(path))) { return; }
#ifdef _WIN32
        (void)lease_protocol;
        (void)RemoveDirectoryW(path.c_str());
#else
        if (!lease_protocol) { (void)::rmdir(path.c_str()); return; }
        int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0 && errno == EACCES) {
            struct stat status{};
            if (::lstat(path.c_str(), &status) == 0 && status.st_uid == ::geteuid() &&
                matches(Private_directory_identity(path)))
            {
                // A restrictive umask can remove owner access. Restore only
                // owner permissions so rollback can acquire the inode lock.
                if (::chmod(path.c_str(), 0700) == 0) {
                    fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                }
            }
        }
        if (fd < 0) { return; }
        struct stat opened{};
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0 && ::fstat(fd, &opened) == 0 &&
            device == static_cast<std::uint64_t>(opened.st_dev) &&
            inode == static_cast<std::uint64_t>(opened.st_ino) &&
            matches(Private_directory_identity(path)))
        {
            (void)::rmdir(path.c_str());
        }
        ::close(fd);
#endif
    }
};

inline bool create_private_directory(const std::filesystem::path& directory)
{
    bool created = false;
#ifdef _WIN32
    Private_security security(true);
    created = CreateDirectoryW(directory.c_str(), security.attributes()) != 0;
    if (!created &&
        GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return false;
    }
#else
    created = mkdir(directory.c_str(), 0700) == 0;
    if (!created && errno != EEXIST) {
        return false;
    }
#endif
    const Private_directory_identity created_identity(directory);
    // Never adopt another account's directory, a link, or a permissive leftover.
    if (private_directory_owned(directory)) {
        return true;
    }
    // Only an empty entry created by this attempt may be rolled back. Existing
    // entries, including collisions owned by another account, stay untouched.
    if (created) { created_identity.remove_empty(directory); }
    return false;
}

#ifdef _WIN32
inline std::filesystem::path private_swarm_root(std::uint64_t swarm_id = 0, bool lease = false)
#else
inline std::filesystem::path private_swarm_root(std::uint64_t swarm_id, bool lease = false)
#endif
{
#ifdef _WIN32
    // Name the account by a 64-bit FNV-1a digest of its SID. A domain SID
    // string is about 30 characters longer, and ring paths must stay within
    // MAX_PATH. A digest collision only fails the ownership check; it grants
    // no access.
    Current_user user;
    const auto* sid = static_cast<const unsigned char*>(user.sid());
    std::uint64_t digest = 0xcbf29ce484222325ull;
    for (DWORD i = 0, length = GetLengthSid(user.sid()); i < length; ++i) {
        digest = (digest ^ sid[i]) * 0x100000001b3ull;
    }
    (void)lease;
    char component[48];
    if (swarm_id == 0) {
        std::snprintf(component, sizeof(component), "sintra-%016llx",
            static_cast<unsigned long long>(digest));
    }
    else {
        std::snprintf(component, sizeof(component), "sintra-%016llx-%016llx",
            static_cast<unsigned long long>(digest), static_cast<unsigned long long>(swarm_id));
    }
#else
    char component[48];
    std::snprintf(component, sizeof(component), lease ? "sintra-l1-%llu-%016llx" : "sintra-%llu-%016llx",
        static_cast<unsigned long long>(geteuid()),
        static_cast<unsigned long long>(swarm_id));
#endif
    return std::filesystem::temp_directory_path() / component;
}

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline std::uint64_t (*draw_private_swarm_id_for_test)() = nullptr;
#endif

inline std::uint64_t draw_private_swarm_id()
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (draw_private_swarm_id_for_test) {
        return draw_private_swarm_id_for_test();
    }
#endif
    std::uint64_t id = 0;
#ifdef _WIN32
    using Random_fn = BOOLEAN (APIENTRY*)(PVOID, ULONG);
    const auto module = LoadLibraryW(L"advapi32.dll");
    const auto random = module ? reinterpret_cast<Random_fn>(
        GetProcAddress(module, "SystemFunction036")) : nullptr;
    const bool ok = random && random(&id, sizeof(id));
    if (module) { FreeLibrary(module); }
    if (!ok) {
        throw std::runtime_error("Sintra could not draw a random swarm identifier.");
    }
#else
    const int random = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random < 0) {
        throw std::system_error(errno, std::system_category(), "open /dev/urandom");
    }
    auto* bytes = reinterpret_cast<unsigned char*>(&id);
    std::size_t remaining = sizeof(id);
    while (remaining != 0) {
        const auto count = ::read(random, bytes, remaining);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int error = count == 0 ? EIO : errno;
            ::close(random);
            throw std::system_error(error, std::system_category(), "read /dev/urandom");
        }
        bytes += count;
        remaining -= static_cast<std::size_t>(count);
    }
    ::close(random);
#endif
    return id;
}

enum class private_swarm_create_result { created, collision, failed };

inline private_swarm_create_result create_private_swarm_directory_exclusive(
    const std::filesystem::path& directory, Private_directory_identity* identity = nullptr,
    bool lease_protocol = false)
{
#ifdef _WIN32
    Private_security security(true);
    if (!CreateDirectoryW(directory.c_str(), security.attributes())) {
        return GetLastError() == ERROR_ALREADY_EXISTS ? private_swarm_create_result::collision :
            private_swarm_create_result::failed;
    }
#else
    if (::mkdir(directory.c_str(), 0700) != 0) {
        return errno == EEXIST ? private_swarm_create_result::collision :
            private_swarm_create_result::failed;
    }
#endif
    const Private_directory_identity created_identity(directory);
    if (private_directory_owned(directory) &&
        created_identity.matches(Private_directory_identity(directory)))
    {
        if (identity) { *identity = created_identity; }
        return private_swarm_create_result::created;
    }
    if (!created_identity.matches(Private_directory_identity(directory))) {
        return private_swarm_create_result::collision;
    }
    created_identity.remove_empty(directory, lease_protocol);
    return private_swarm_create_result::failed;
}

// The directory is owner-only. A direct POSIX swarm root also needs a trusted
// temporary parent, with sticky protection when other accounts can write it.
// Never recurse through links or remove files whose ownership is unverified.
inline bool remove_private_directory_tree(const std::filesystem::path& directory,
    bool remove_root = true) noexcept
try
{
    if (!private_directory_owned(directory)) {
        return false;
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        if (private_directory_owned(it->path())) {
            if (!remove_private_directory_tree(it->path())) {
                return false;
            }
        }
        else if (!private_file_path_owned(it->path()) || !std::filesystem::remove(it->path(), ec)) {
            return false;
        }
    }
    return !ec && (!remove_root || std::filesystem::remove(directory, ec));
}
catch (...) {
    return false;
}

#ifdef _WIN32
// Pin the exact root name during classification and removal. Other opens must
// share delete access; another deleting/renaming handle cannot coexist.
class Private_directory_removal
{
public:
    explicit Private_directory_removal(const std::filesystem::path& directory,
        const Private_directory_identity* expected = nullptr) : m_directory(directory)
    {
        m_handle = CreateFileW(directory.c_str(), DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) { return; }
        bool owned = false;
        try { owned = private_object_owned(m_handle); }
        catch (...) { CloseHandle(m_handle); m_handle = INVALID_HANDLE_VALUE; throw; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(m_handle, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !owned ||
            (expected && (!expected->valid || expected->device != info.dwVolumeSerialNumber ||
                expected->inode != ((static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow))))
        {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }
    Private_directory_removal(const Private_directory_removal&) = delete;
    Private_directory_removal& operator=(const Private_directory_removal&) = delete;
    ~Private_directory_removal()
    {
        if (valid()) { CloseHandle(m_handle); }
    }
    bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }
    bool remove()
    {
        if (!valid() || !remove_private_directory_tree(m_directory, false)) { return false; }
        FILE_DISPOSITION_INFO disposition{TRUE};
        return SetFileInformationByHandle(m_handle, FileDispositionInfo,
            &disposition, sizeof(disposition)) != 0;
    }
private:
    std::filesystem::path m_directory;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};
#endif

} // namespace sintra::detail
