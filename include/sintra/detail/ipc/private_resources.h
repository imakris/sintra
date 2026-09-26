// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
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

inline bool create_private_directory(const std::filesystem::path& directory)
{
#ifdef _WIN32
    Private_security security(true);
    if (!CreateDirectoryW(directory.c_str(), security.attributes()) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return false;
    }
#else
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }
#endif
    // Never adopt another account's directory, a link, or a permissive leftover.
    return private_directory_owned(directory);
}

inline std::filesystem::path private_swarm_root()
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
    char component[24];
    std::snprintf(component, sizeof(component), "sintra-%016llx",
        static_cast<unsigned long long>(digest));
#else
    const auto component = "sintra-" + std::to_string(geteuid());
#endif
    return std::filesystem::temp_directory_path() / component;
}

// The caller owns a private parent, so other accounts cannot replace its entries.
// Never recurse through links or remove files whose ownership is unverified.
inline bool remove_private_directory_tree(const std::filesystem::path& directory) noexcept
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
    return !ec && std::filesystem::remove(directory, ec);
}
catch (...) {
    return false;
}

} // namespace sintra::detail
