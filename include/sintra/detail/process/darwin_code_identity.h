// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "managed_process.h"

#include <Security/Security.h>
#include <bsm/libbsm.h>
#include <limits.h>
#include <mach/machine.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <string>
#include <utility>

namespace sintra::detail {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace test_hooks {
inline std::atomic<void(*)()> s_darwin_static_validation_hook{nullptr};
}
#endif

template<typename T>
class Darwin_code_reference
{
public:
    Darwin_code_reference() = default;
    Darwin_code_reference(const Darwin_code_reference&) = delete;
    Darwin_code_reference& operator=(const Darwin_code_reference&) = delete;
    ~Darwin_code_reference() { if (m_value) CFRelease(m_value); }

    T get() const { return m_value; }
    T* out() { return &m_value; }
    T release() { return std::exchange(m_value, nullptr); }

private:
    T m_value = nullptr;
};

inline cpu_type_t darwin_code_architecture()
{
#if defined(__arm64__) || defined(__aarch64__)
    return CPU_TYPE_ARM64;
#elif defined(__x86_64__)
    return CPU_TYPE_X86_64;
#else
    return 0;
#endif
}

inline bool darwin_same_capture(const struct stat& first, const struct stat& second)
{
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino &&
        first.st_size == second.st_size && first.st_mode == second.st_mode &&
        first.st_mtimespec.tv_sec == second.st_mtimespec.tv_sec &&
        first.st_mtimespec.tv_nsec == second.st_mtimespec.tv_nsec &&
        first.st_ctimespec.tv_sec == second.st_ctimespec.tv_sec &&
        first.st_ctimespec.tv_nsec == second.st_ctimespec.tv_nsec;
}

inline OSStatus darwin_static_code(const std::string& path, SecStaticCodeRef* code)
{
    struct stat information{};
    if (stat(path.c_str(), &information) != 0) {
        return errSecCSStaticCodeNotFound;
    }
    Darwin_code_reference<CFURLRef> url;
    *url.out() = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()), (CFIndex)path.size(), S_ISDIR(information.st_mode));
    const auto architecture = darwin_code_architecture();
    if (!architecture || !url.get()) {
        return errSecCSBadObjectFormat;
    }
    Darwin_code_reference<CFNumberRef> number;
    *number.out() = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &architecture);
    if (!number.get()) {
        return errSecAllocate;
    }
    const void* keys[]   = {kSecCodeAttributeArchitecture};
    const void* values[] = {number.get()};
    Darwin_code_reference<CFDictionaryRef> attributes;
    *attributes.out() = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attributes.get()) {
        return errSecAllocate;
    }
    return SecStaticCodeCreateWithPathAndAttributes(url.get(), kSecCSDefaultFlags, attributes.get(), code);
}

inline std::string darwin_code_hash(CFDictionaryRef information)
{
    const auto hash = static_cast<CFDataRef>(CFDictionaryGetValue(information, kSecCodeInfoUnique));
    if (!hash || CFGetTypeID(hash) != CFDataGetTypeID() || CFDataGetLength(hash) != 20) {
        return {};
    }
    constexpr char k_hex[] = "0123456789abcdef";
    std::string text;
    const auto* bytes = CFDataGetBytePtr(hash);
    for (CFIndex index = 0; index < CFDataGetLength(hash); ++index) {
        text += k_hex[bytes[index] >> 4];
        text += k_hex[bytes[index] & 15];
    }
    return text;
}

inline std::string darwin_url_path(CFURLRef url)
{
    if (!url || CFGetTypeID(url) != CFURLGetTypeID()) {
        return {};
    }
    UInt8 path[PATH_MAX]{};
    if (!CFURLGetFileSystemRepresentation(url, true, path, sizeof(path))) {
        return {};
    }
    return reinterpret_cast<const char*>(path);
}

inline Managed_child_native_error capture_darwin_code(
    int descriptor, const std::string& selected_path, const struct stat& selected,
    SecRequirementRef* requirement)
{
    using Domain = Managed_child_native_error_domain;
    auto security_error = [](OSStatus status, const char* operation) {
        return Managed_child_native_error{Domain::PROVIDER, (int)status, operation};
    };
    // The architecture selector is ignored for scripts. Require Mach-O before
    // asking Security to select and validate the native slice of a fat file.
    uint32_t magic = 0;
    if (pread(descriptor, &magic, sizeof(magic), 0) != sizeof(magic) ||
        (magic != MH_MAGIC_64 && magic != MH_CIGAM_64 && magic != FAT_MAGIC && magic != FAT_CIGAM &&
         magic != FAT_MAGIC_64 && magic != FAT_CIGAM_64))
    {
        return security_error(errSecCSBadObjectFormat, "Selected executable must contain native Mach-O code");
    }
    Darwin_code_reference<SecStaticCodeRef> retained;
    auto status = darwin_static_code("/dev/fd/" + std::to_string(descriptor), retained.out());
    if (status != errSecSuccess) {
        return security_error(status, "SecStaticCodeCreateWithPathAndAttributes(retained native executable)");
    }
    Darwin_code_reference<CFDictionaryRef> retained_information;
    status = SecCodeCopySigningInformation(retained.get(), kSecCSSigningInformation, retained_information.out());
    if (status != errSecSuccess) {
        return security_error(status, "SecCodeCopySigningInformation(retained executable)");
    }
    const auto hash = darwin_code_hash(retained_information.get());
    if (hash.empty()) {
        return security_error(errSecCSBadObjectFormat, "Selected executable lacks a native CodeDirectory identity");
    }

    std::error_code path_error;
    const auto canonical = std::filesystem::canonical(selected_path, path_error);
    if (path_error) {
        return {Domain::POSIX, path_error.value(), "canonical(selected executable)"};
    }
    // Security recognizes bundle mains; recreate from its canonical normal code
    // URL so full validation has the bundle's Info.plist and resource context.
    Darwin_code_reference<SecStaticCodeRef> selection;
    status = darwin_static_code(canonical.string(), selection.out());
    if (status != errSecSuccess) {
        return security_error(status, "SecStaticCodeCreateWithPathAndAttributes(selected executable)");
    }
    Darwin_code_reference<CFURLRef> code_url;
    status = SecCodeCopyPath(selection.get(), kSecCSDefaultFlags, code_url.out());
    if (status != errSecSuccess) {
        return security_error(status, "SecCodeCopyPath(selected code)");
    }
    const auto code_path = std::filesystem::canonical(darwin_url_path(code_url.get()), path_error);
    if (path_error) {
        return {Domain::POSIX, path_error.value(), "canonical(selected code)"};
    }
    Darwin_code_reference<SecStaticCodeRef> complete;
    status = darwin_static_code(code_path.string(), complete.out());
    if (status != errSecSuccess) {
        return security_error(status, "SecStaticCodeCreateWithPathAndAttributes(complete selected code)");
    }
    Darwin_code_reference<CFDictionaryRef> information;
    status = SecCodeCopySigningInformation(complete.get(), kSecCSSigningInformation, information.out());
    if (status != errSecSuccess) {
        return security_error(status, "SecCodeCopySigningInformation(complete selected code)");
    }
    const auto main_url = static_cast<CFURLRef>(CFDictionaryGetValue(information.get(), kSecCodeInfoMainExecutable));
    const auto main_path = darwin_url_path(main_url);
    struct stat main_before{}, path_before{};
    if (main_path.empty() || stat(main_path.c_str(), &main_before) != 0 ||
        stat(selected_path.c_str(), &path_before) != 0)
    {
        return security_error(errSecCSNoMainExecutable, "Complete selected code main executable unavailable");
    }
    if (!darwin_same_capture(selected, main_before) || !darwin_same_capture(selected, path_before) ||
        darwin_code_hash(information.get()) != hash)
    {
        return security_error(errSecCSStaticCodeChanged, "Complete code differs from retained selection");
    }
    const std::string expression = "cdhash H\"" + hash + "\"";
    Darwin_code_reference<CFStringRef> text;
    *text.out() = CFStringCreateWithCString(kCFAllocatorDefault, expression.c_str(), kCFStringEncodingUTF8);
    if (!text.get()) {
        return security_error(errSecAllocate, "CFStringCreateWithCString(exact code requirement)");
    }
    Darwin_code_reference<SecRequirementRef> exact;
    status = SecRequirementCreateWithString(text.get(), kSecCSDefaultFlags, exact.out());
    if (status != errSecSuccess) {
        return security_error(status, "SecRequirementCreateWithString(exact code requirement)");
    }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (const auto hook = test_hooks::s_darwin_static_validation_hook.load()) {
        hook();
    }
#endif
    status = SecStaticCodeCheckValidity(complete.get(),
        kSecCSStrictValidate | kSecCSCheckNestedCode | kSecCSSingleThreaded, exact.get());
    if (status != errSecSuccess) {
        return security_error(status, "SecStaticCodeCheckValidity(complete selected code)");
    }
    struct stat retained_after{}, main_after{}, path_after{};
    if (fstat(descriptor, &retained_after) != 0 || stat(main_path.c_str(), &main_after) != 0 ||
        stat(selected_path.c_str(), &path_after) != 0)
    {
        return {Domain::POSIX, errno, "stat(selected executable after static validation)"};
    }
    if (!darwin_same_capture(selected, retained_after) || !darwin_same_capture(selected, main_after) ||
        !darwin_same_capture(selected, path_after))
    {
        return security_error(errSecCSStaticCodeChanged, "Selected executable changed during static validation");
    }
    *requirement = exact.release();
    return {};
}

template<typename Result, typename Liveness>
inline Managed_child_native_peer_proof verify_darwin_code(
    uint64_t native_pid, uint64_t start_stamp, SecRequirementRef requirement,
    const audit_token_t* audit, Result result, Liveness liveness)
{
    using State  = Managed_child_native_peer_state;
    using Domain = Managed_child_native_error_domain;
    Darwin_code_reference<CFNumberRef> pid_number;
    Darwin_code_reference<CFDataRef> audit_data;
    const auto pid = (pid_t)native_pid;
    const void* key = nullptr;
    const void* value = nullptr;
    if (audit) {
        *audit_data.out() = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(audit), sizeof(*audit));
        key   = kSecGuestAttributeAudit;
        value = audit_data.get();
    }
    else {
        *pid_number.out() = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
        key   = kSecGuestAttributePid;
        value = pid_number.get();
    }
    if (!value) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, errSecAllocate, "Allocate dynamic code identity"});
    }
    Darwin_code_reference<CFDictionaryRef> attributes;
    *attributes.out() = CFDictionaryCreate(kCFAllocatorDefault, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attributes.get()) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, errSecAllocate, "Allocate dynamic code attributes"});
    }
    Darwin_code_reference<SecCodeRef> guest;
    auto status = SecCodeCopyGuestWithAttributes(nullptr, attributes.get(), kSecCSDefaultFlags, guest.out());
    const char* operation = "SecCodeCopyGuestWithAttributes(original occurrence)";
    if (status == errSecSuccess) {
        status = SecCodeCheckValidity(guest.get(), kSecCSDefaultFlags, requirement);
        operation = "SecCodeCheckValidity(original occurrence, exact code requirement)";
    }
    const auto final = liveness();
    if (final.state != State::MATCH) {
        return final;
    }
    if (status != errSecSuccess) {
        const auto state = status == errSecCSReqFailed || status == errSecCSGuestInvalid ||
            status == errSecCSSignatureFailed || status == errSecCSUnsigned ? State::MISMATCH : State::UNAVAILABLE;
        return result(state, {Domain::PROVIDER, (int)status, operation});
    }
    auto proof = result(State::MATCH);
    proof.native_process_id                = native_pid;
    proof.native_process_creation_identity = start_stamp;
    return proof;
}

} // namespace sintra::detail
