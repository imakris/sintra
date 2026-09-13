// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "managed_process.h"
#include "../ipc/process_utils.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace sintra::detail {

class Managed_child_native_reference
{
public:
#ifdef _WIN32
    explicit Managed_child_native_reference(HANDLE handle) : m_handle(handle) {}
    Managed_child_native_reference(Managed_child_native_reference&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)) {}
    ~Managed_child_native_reference() { if (m_handle) CloseHandle(m_handle); }
    HANDLE handle() const { return m_handle; }
#elif defined(__linux__)
    explicit Managed_child_native_reference(int descriptor) : m_descriptor(descriptor) {}
    Managed_child_native_reference(Managed_child_native_reference&& other) noexcept
        : m_descriptor(std::exchange(other.m_descriptor, -1)) {}
    ~Managed_child_native_reference() { if (m_descriptor >= 0) close(m_descriptor); }
    int descriptor() const { return m_descriptor; }
#endif
    Managed_child_native_reference(const Managed_child_native_reference&) = delete;
    Managed_child_native_reference& operator=(const Managed_child_native_reference&) = delete;

private:
#ifdef _WIN32
    HANDLE m_handle;
#elif defined(__linux__)
    int m_descriptor;
#endif
};

#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace test_hooks {
using Native_peer_inspection_error = int(*)();
inline std::atomic<Native_peer_inspection_error> s_native_peer_inspection_error{nullptr};
}
#endif

inline int native_peer_inspection_error_for_test()
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (const auto hook = test_hooks::s_native_peer_inspection_error.load()) {
        return hook();
    }
#endif
    return 0;
}

#ifdef _WIN32
inline Managed_child_native_peer_state native_image_mapping_state(LONG status)
{
    // ProcessImageFileMapping is an undocumented native information class.
    // Its defined nonmatching-file result is STATUS_UNSUCCESSFUL. Never turn
    // unsupported classes, denied access, or other native failures into proof.
    if (status == 0) {
        return Managed_child_native_peer_state::MATCH;
    }
    if (static_cast<uint32_t>(status) == 0xc0000001u) {
        return Managed_child_native_peer_state::MISMATCH;
    }
    return Managed_child_native_peer_state::UNAVAILABLE;
}
#endif

template<typename Result, typename Liveness>
inline Managed_child_native_peer_proof verify_native_executable_image(
#ifdef _WIN32
    HANDLE process,
#else
    uint64_t native_pid,
    uint64_t start_stamp,
#endif
    const Managed_child_native_reference& executable,
    Result result,
    Liveness liveness)
{
    using State  = Managed_child_native_peer_state;
    using Domain = Managed_child_native_error_domain;
#ifdef _WIN32
    // Mirror only the native call's scalar ABI. Including winternl.h here
    // requires OPTIONAL even when a consumer deliberately cleaned that macro
    // after windows.h; restoring it would leak into the consumer's public APIs.
    using Query = LONG(WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<Query>(GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) {
        return result(State::UNAVAILABLE, {Domain::WINDOWS, ERROR_PROC_NOT_FOUND, "NtQueryInformationProcess unavailable"});
    }
    const auto native_pid = GetProcessId(process);
    if (native_pid == 0) {
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, (int)GetLastError(), "GetProcessId(original native proof)"});
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, (int)GetLastError(), "GetProcessTimes(original native proof)"});
    }
    ULARGE_INTEGER creation_identity{};
    creation_identity.LowPart  = creation.dwLowDateTime;
    creation_identity.HighPart = creation.dwHighDateTime;
    HANDLE image = executable.handle();
    // Class 44 takes an input file HANDLE and compares the process image's file
    // object. Path queries/reopening cannot substitute: rename and replacement
    // must not make an unrelated file satisfy the selected payload identity.
    const auto injected = detail::native_peer_inspection_error_for_test();
    const auto status = injected != 0 ? (LONG)injected :
        query(process, 44ul, &image, sizeof(image), nullptr);
    const auto final = liveness();
    if (final.state != State::MATCH) {
        return final;
    }
    const auto state = detail::native_image_mapping_state(status);
    if (state != State::MATCH) {
        return result(state,
            {Domain::WINDOWS, (int)status, "NtQueryInformationProcess(ProcessImageFileMapping): NTSTATUS"});
    }
    auto proof = result(State::MATCH);
    proof.native_process_id                = native_pid;
    proof.native_process_creation_identity = creation_identity.QuadPart;
    return proof;
#elif defined(__linux__)
    const auto proc_image = "/proc/" + std::to_string(native_pid) + "/exe";
    const auto injected = detail::native_peer_inspection_error_for_test();
    const int descriptor = injected != 0 ? -1 : open(proc_image.c_str(), O_PATH | O_CLOEXEC);
    if (descriptor < 0) {
        const auto error = injected != 0 ? injected : errno;
        const auto final = liveness();
        return final.state != State::MATCH ? final : result(State::UNAVAILABLE,
            {Domain::POSIX, error, "open(original child executable)"});
    }
    detail::Managed_child_native_reference image_owner(descriptor);
    struct stat expected{};
    struct stat actual{};
    if (fstat(executable.descriptor(), &expected) != 0 || fstat(descriptor, &actual) != 0) {
        return result(State::UNAVAILABLE, {Domain::POSIX, errno, "fstat(native executable identity)"});
    }
    const auto final = liveness();
    if (final.state != State::MATCH) {
        return final;
    }
    if (expected.st_dev != actual.st_dev || expected.st_ino != actual.st_ino) {
        return result(State::MISMATCH, {Domain::PROVIDER, 0, "Loaded executable file differs from selection"});
    }
    auto proof = result(State::MATCH);
    proof.native_process_id                = (uint64_t)native_pid;
    proof.native_process_creation_identity = start_stamp;
    return proof;
#else
    (void)native_pid;
    (void)start_stamp;
    (void)executable;
    (void)liveness;
    return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native executable identity unsupported"});
#endif
}

} // namespace sintra::detail

namespace sintra {

inline Managed_child_executable_capture capture_managed_child_executable(const std::string& path)
{
    using Domain = Managed_child_native_error_domain;
    Managed_child_executable_capture result;
    if (path.empty() || path.find('\0') != std::string::npos ||
        !std::filesystem::u8path(path).is_absolute())
    {
        result.error = {Domain::PROVIDER, 0, "Selected executable path must be absolute"};
        return result;
    }
#ifdef _WIN32
    const auto native_path = std::filesystem::u8path(path);
    const auto handle = CreateFileW(native_path.c_str(), GENERIC_READ | GENERIC_EXECUTE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = {Domain::WINDOWS, static_cast<int>(GetLastError()), "CreateFile(selected executable)"};
        return result;
    }
    detail::Managed_child_native_reference owned(handle);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        result.error = {Domain::WINDOWS, static_cast<int>(GetLastError()), "GetFileInformationByHandle(selected executable)"};
        return result;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.error = {Domain::PROVIDER, 0, "Selected executable is not a regular file"};
        return result;
    }
    result.reference.m_state = std::make_shared<detail::Managed_child_native_reference>(std::move(owned));
#elif defined(__linux__)
    const int descriptor = open(path.c_str(), O_PATH | O_CLOEXEC);
    if (descriptor < 0) {
        result.error = {Domain::POSIX, errno, "open(selected executable)"};
        return result;
    }
    detail::Managed_child_native_reference owned(descriptor);
    struct stat information{};
    if (fstat(descriptor, &information) != 0) {
        result.error = {Domain::POSIX, errno, "fstat(selected executable)"};
        return result;
    }
    if (!S_ISREG(information.st_mode)) {
        result.error = {Domain::PROVIDER, 0, "Selected executable is not a regular file"};
        return result;
    }
    result.reference.m_state = std::make_shared<detail::Managed_child_native_reference>(std::move(owned));
#else
    result.error = {Domain::PROVIDER, 0, "Native executable identity unsupported"};
#endif
    return result;
}

inline bool compare_managed_child_executables(
    const Managed_child_executable_reference& first,
    const Managed_child_executable_reference& second)
{
    if (!first.m_state || !second.m_state) {
        return false;
    }
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION first_information{};
    BY_HANDLE_FILE_INFORMATION second_information{};
    if (!GetFileInformationByHandle(first.m_state->handle(), &first_information) ||
        !GetFileInformationByHandle(second.m_state->handle(), &second_information))
    {
        return false;
    }
    return first_information.dwVolumeSerialNumber == second_information.dwVolumeSerialNumber &&
        first_information.nFileIndexHigh == second_information.nFileIndexHigh &&
        first_information.nFileIndexLow == second_information.nFileIndexLow;
#elif defined(__linux__)
    struct stat first_information{};
    struct stat second_information{};
    if (fstat(first.m_state->descriptor(), &first_information) != 0 ||
        fstat(second.m_state->descriptor(), &second_information) != 0)
    {
        return false;
    }
    return first_information.st_dev == second_information.st_dev &&
        first_information.st_ino == second_information.st_ino;
#else
    return false;
#endif
}

inline Managed_child_native_peer_proof verify_process_executable(
    uint64_t native_pid, const Managed_child_executable_reference& executable)
{
    using State  = Managed_child_native_peer_state;
    using Domain = Managed_child_native_error_domain;
    auto result = [](State state, Managed_child_native_error error = {}) {
        return Managed_child_native_peer_proof{state, {}, std::move(error)};
    };
    if (!executable.m_state) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Selected executable reference unavailable"});
    }
#ifdef _WIN32
    if (native_pid == 0 || native_pid > std::numeric_limits<DWORD>::max()) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Invalid native process identity"});
    }
    const auto process = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
        FALSE, static_cast<DWORD>(native_pid));
    if (!process) {
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, static_cast<int>(GetLastError()), "OpenProcess(caller-owned child)"});
    }
    detail::Managed_child_native_reference process_owner(process);
    auto liveness = [&]() {
        const auto observed = WaitForSingleObject(process, 0);
        if (observed == WAIT_OBJECT_0) return result(State::EXITED);
        if (observed == WAIT_TIMEOUT)  return result(State::MATCH);
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, static_cast<int>(GetLastError()), "WaitForSingleObject(native proof)"});
    };
    const auto initial = liveness();
    if (initial.state != State::MATCH) {
        return initial;
    }
    return detail::verify_native_executable_image(process, *executable.m_state, result, liveness);
#elif defined(__linux__)
    if (native_pid == 0 || native_pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Invalid native process identity"});
    }
    const auto pid = static_cast<pid_t>(native_pid);
    auto liveness = [&]() {
        siginfo_t information{};
        int observed;
        do {
            observed = waitid(P_PID, pid, &information, WEXITED | WNOHANG | WNOWAIT | __WALL);
        }
        while (observed < 0 && errno == EINTR);
        if (observed < 0) {
            return result(State::UNAVAILABLE, {Domain::POSIX, errno, "waitid(caller-owned child, WNOWAIT)"});
        }
        return result(information.si_pid == pid ? State::EXITED : State::MATCH);
    };
    const auto initial = liveness();
    if (initial.state != State::MATCH) {
        return initial;
    }
    const auto stamp = query_process_start_stamp(static_cast<uint32_t>(pid));
    if (!stamp) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native creation identity unavailable"});
    }
    auto stable_liveness = [&]() {
        const auto live = liveness();
        if (live.state != State::MATCH) {
            return live;
        }
        const auto current = query_process_start_stamp(static_cast<uint32_t>(pid));
        if (!current || *current != *stamp) {
            return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native creation identity changed"});
        }
        return live;
    };
    return detail::verify_native_executable_image(native_pid, *stamp,
        *executable.m_state, result, stable_liveness);
#else
    (void)native_pid;
    return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native executable identity unsupported"});
#endif
}

inline Managed_child_native_peer_proof Managed_process::verify_child_native_peer(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    const Managed_child_occurrence_identity& identity,
    uintptr_t connected_server_endpoint,
    const Managed_child_executable_reference& executable)
{
    using State  = Managed_child_native_peer_state;
    using Domain = Managed_child_native_error_domain;
    auto result = [&](State state, Managed_child_native_error error = {}) {
        return Managed_child_native_peer_proof{state, identity, std::move(error)};
    };

#if defined(__linux__)
    // Same order as native handoff and the sole reaper. Holding this roster
    // prevents reaping/PID reuse throughout peer and /proc image inspection.
    std::lock_guard<std::mutex> roster_lock(m_spawned_child_pids_mutex);
#endif
    std::lock_guard<std::mutex> custody_lock(custody->mutex);
    if (identity.custody_identity == 0 || identity.custody_identity != custody->identity) {
        return result(State::MISMATCH, {Domain::PROVIDER, 0, "Native custody identity mismatch"});
    }
    const auto* occurrence = custody->find_occurrence_locked(identity.process_instance_id, identity.occurrence);
    if (!occurrence) {
        return result(State::MISMATCH, {Domain::PROVIDER, 0, "Native occurrence identity mismatch"});
    }
    if (occurrence->native.exited()) {
        return result(State::EXITED);
    }
    if (!occurrence->native.running() ||
        occurrence->setup != detail::Managed_child_occurrence_record::setup_state::ownership_ready)
    {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Exact native ownership is not ready"});
    }
    if (!executable.m_state) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Selected executable reference unavailable"});
    }

#ifdef _WIN32
    const auto process = reinterpret_cast<HANDLE>(occurrence->native.process_handle());
    if (!occurrence->native.process_handle_owned()) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Retained process handle unavailable"});
    }
    auto liveness = [&]() {
        const auto observed = WaitForSingleObject(process, 0);
        if (observed == WAIT_OBJECT_0) return result(State::EXITED);
        if (observed == WAIT_TIMEOUT)  return result(State::MATCH);
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, static_cast<int>(GetLastError()), "WaitForSingleObject(native proof)"});
    };
    const auto initial = liveness();
    if (initial.state != State::MATCH) {
        return initial;
    }
    const auto pipe = reinterpret_cast<HANDLE>(connected_server_endpoint);
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr)) {
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, static_cast<int>(GetLastError()), "PeekNamedPipe(native proof)"});
    }
    ULONG peer_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &peer_pid)) {
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, static_cast<int>(GetLastError()), "GetNamedPipeClientProcessId"});
    }
    // Opening only the OS-authenticated peer is not authority. The comparison
    // below binds that handle to the already retained original process object.
    const auto peer = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer_pid);
    if (!peer) {
        return result(State::UNAVAILABLE,
            {Domain::WINDOWS, static_cast<int>(GetLastError()), "OpenProcess(authenticated pipe peer)"});
    }
    detail::Managed_child_native_reference peer_owner(peer);
    using Compare = BOOL(WINAPI*)(HANDLE, HANDLE);
    const auto compare = reinterpret_cast<Compare>(GetProcAddress(
        GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles"));
    if (!compare) {
        return result(State::UNAVAILABLE, {Domain::WINDOWS, ERROR_PROC_NOT_FOUND, "CompareObjectHandles unavailable"});
    }
    if (!compare(process, peer)) {
        const auto error = GetLastError();
        return result(error == ERROR_NOT_SAME_OBJECT ? State::MISMATCH : State::UNAVAILABLE,
            {Domain::WINDOWS, static_cast<int>(error), "CompareObjectHandles(authenticated peer)"});
    }
    return detail::verify_native_executable_image(process, *executable.m_state, result, liveness);
#elif defined(__linux__)
    const auto slot = std::find_if(m_spawned_child_pids.begin(), m_spawned_child_pids.end(),
        [&](const Spawned_child_reap_slot& candidate) {
            return candidate.pid == occurrence->native.pid() &&
                candidate.occurrence.custody.lock() == custody &&
                candidate.occurrence.process_instance_id == identity.process_instance_id &&
                candidate.occurrence.occurrence == identity.occurrence;
        });
    if (slot == m_spawned_child_pids.end() || !slot->start_stamp_available) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Exact native reap slot unavailable"});
    }
    auto liveness = [&]() {
        siginfo_t information{};
        int observed;
        do {
            observed = waitid(P_PID, slot->pid, &information, WEXITED | WNOHANG | WNOWAIT | __WALL);
        }
        while (observed < 0 && errno == EINTR);
        if (observed < 0) {
            return result(State::UNAVAILABLE, {Domain::POSIX, errno, "waitid(native proof, WNOWAIT)"});
        }
        return result(information.si_pid == slot->pid ? State::EXITED : State::MATCH);
    };
    const auto initial = liveness();
    if (initial.state != State::MATCH) {
        return initial;
    }
    if (connected_server_endpoint > static_cast<uintptr_t>(std::numeric_limits<int>::max())) {
        return result(State::UNAVAILABLE, {Domain::POSIX, EBADF, "Native peer socket descriptor"});
    }
    const int socket = static_cast<int>(connected_server_endpoint);
    pollfd connection{socket, POLLIN | POLLRDHUP, 0};
    const int connection_result = poll(&connection, 1, 0);
    if (connection_result < 0) {
        return result(State::UNAVAILABLE, {Domain::POSIX, errno, "poll(native peer connection)"});
    }
    if ((connection.revents & (POLLHUP | POLLRDHUP | POLLERR | POLLNVAL)) != 0) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native peer connection unavailable"});
    }
    sockaddr_un address{};
    socklen_t address_size = sizeof(address);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
        return result(State::UNAVAILABLE, {Domain::POSIX, errno, "getpeername(native proof)"});
    }
    if (address.sun_family != AF_UNIX) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native proof requires AF_UNIX peer"});
    }
    ucred peer{};
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0) {
        return result(State::UNAVAILABLE, {Domain::POSIX, errno, "getsockopt(SO_PEERCRED)"});
    }
    if (peer_size != sizeof(peer)) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native peer credential size mismatch"});
    }
    if (peer.pid != slot->pid || peer.uid != geteuid()) {
        return result(State::MISMATCH, {Domain::PROVIDER, 0, "Authenticated socket peer differs from child"});
    }
    const auto stamp = query_process_start_stamp(static_cast<uint32_t>(slot->pid));
    if (!stamp || *stamp != slot->start_stamp) {
        return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Original native creation identity unavailable"});
    }
    return detail::verify_native_executable_image(
        (uint64_t)slot->pid, slot->start_stamp, *executable.m_state, result, liveness);
#else
    (void)connected_server_endpoint;
    return result(State::UNAVAILABLE, {Domain::PROVIDER, 0, "Native peer identity unsupported"});
#endif
}

} // namespace sintra
