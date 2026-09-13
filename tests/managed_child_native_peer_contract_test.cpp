// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// Public framework headers permanently clean these Windows status macros before
// including Sintra. Exercise that real include order without a framework build
// dependency, and require Sintra to preserve the consumer's cleaned namespace.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// The harness's DbgHelp declarations require the original SDK annotations.
// Load them before simulating the public consumer's permanent macro cleanup.
#include "test_environment.h"
#undef OPTIONAL
#undef FAILED
#undef ERROR
#undef NO_DATA
#undef DATA_AVAILABLE
#undef EVICTED
#endif

#include <sintra/sintra.h>

#if defined(_WIN32) || defined(__linux__)

#if defined(_WIN32) && (defined(OPTIONAL) || defined(FAILED) || defined(ERROR) || \
    defined(NO_DATA) || defined(DATA_AVAILABLE) || defined(EVICTED))
#error Sintra must preserve cleaned Windows status macros
#endif

#include "test_utils.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using State = sintra::Managed_child_native_peer_state;
using std::chrono_literals::operator""s;

constexpr auto k_child_iid = sintra::compose_instance(57u, 1ull);
constexpr const char* k_child_flag = "--native-peer-child";
constexpr const char* k_endpoint_flag = "--native-peer-endpoint";
constexpr const char* k_control_flag = "--native-peer-control";
constexpr const char* k_exec_path_flag = "--native-peer-exec-path";

void require(bool condition, const char* expectation)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", expectation);
        throw std::runtime_error(expectation);
    }
}

#ifdef _WIN32
using Native_endpoint = HANDLE;
const Native_endpoint k_invalid_endpoint = INVALID_HANDLE_VALUE;
void close_endpoint(Native_endpoint endpoint) { CloseHandle(endpoint); }

bool exchange_byte(Native_endpoint endpoint, char& value, bool send, bool overlapped)
{
    DWORD transferred = 0;
    OVERLAPPED operation{};
    if (overlapped) {
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
    }
    BOOL completed = send
        ? WriteFile(endpoint, &value, 1, &transferred, overlapped ? &operation : nullptr)
        : ReadFile(endpoint, &value, 1, &transferred, overlapped ? &operation : nullptr);
    if (!completed && overlapped && GetLastError() == ERROR_IO_PENDING) {
        completed = WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0 &&
            GetOverlappedResult(endpoint, &operation, &transferred, FALSE);
        if (!completed) {
            CancelIoEx(endpoint, &operation);
            GetOverlappedResult(endpoint, &operation, &transferred, TRUE);
        }
    }
    if (operation.hEvent) CloseHandle(operation.hEvent);
    return completed && transferred == 1;
}

Native_endpoint connect_peer(const std::string& name)
{
    return CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
#elif defined(__linux__)
using Native_endpoint = int;
constexpr Native_endpoint k_invalid_endpoint = -1;
void close_endpoint(Native_endpoint endpoint) { close(endpoint); }

bool exchange_byte(Native_endpoint endpoint, char& value, bool send, bool)
{
    pollfd ready{endpoint, static_cast<short>(send ? POLLOUT : POLLIN), 0};
    if (poll(&ready, 1, 5000) <= 0) return false;
    return send ? ::send(endpoint, &value, 1, MSG_NOSIGNAL) == 1 : recv(endpoint, &value, 1, 0) == 1;
}

sockaddr_un socket_address(const std::string& name)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(name.size() + 1 < sizeof(address.sun_path), "bounded abstract socket name");
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    return address;
}

Native_endpoint connect_peer(const std::string& name)
{
    const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket < 0) return k_invalid_endpoint;
    const auto address = socket_address(name);
    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket);
        return k_invalid_endpoint;
    }
    return socket;
}
#endif

class Endpoint
{
public:
    Endpoint()
    {
        m_name = "sintra_peer_proof_" + std::to_string(sintra::test::get_pid()) + "_" +
            std::to_string(Clock::now().time_since_epoch().count());
#ifdef _WIN32
        m_name = "\\\\.\\pipe\\" + m_name;
#endif
    }

    void listen()
    {
#ifdef _WIN32
        m_listener = CreateNamedPipeA(m_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 128, 128, 0, nullptr);
        require(m_listener != k_invalid_endpoint, "create owned pipe server");
#elif defined(__linux__)
        m_listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(m_listener != k_invalid_endpoint, "create owned Unix listener");
        const auto address = socket_address(m_name);
        require(bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "bind owned Unix listener");
        require(::listen(m_listener, 1) == 0, "listen for actual child peer");
#endif
    }
    ~Endpoint()
    {
        if (m_peer != k_invalid_endpoint && m_peer != m_listener) close_endpoint(m_peer);
        if (m_listener != k_invalid_endpoint) close_endpoint(m_listener);
    }

    void accept_peer()
    {
#ifdef _WIN32
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(operation.hEvent != nullptr, "create pipe connection event");
        bool connected = ConnectNamedPipe(m_listener, &operation) != FALSE;
        if (!connected) {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            }
            else
            if (error == ERROR_IO_PENDING) {
                DWORD transferred = 0;
                connected = WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0 &&
                    GetOverlappedResult(m_listener, &operation, &transferred, FALSE);
                if (!connected) {
                    CancelIoEx(m_listener, &operation);
                    GetOverlappedResult(m_listener, &operation, &transferred, TRUE);
                }
            }
        }
        CloseHandle(operation.hEvent);
        require(connected, "accept actual child pipe connection");
        m_peer = m_listener;
#elif defined(__linux__)
        pollfd ready{m_listener, POLLIN, 0};
        require(poll(&ready, 1, 5000) == 1, "actual child connects before deadline");
        m_peer = accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
        require(m_peer != k_invalid_endpoint, "accept actual child Unix connection");
#endif
    }

    const std::string& name() const { return m_name; }
    uintptr_t borrowed() const
    {
#ifdef _WIN32
        return reinterpret_cast<uintptr_t>(m_peer);
#else
        return static_cast<uintptr_t>(m_peer);
#endif
    }
    void send(char value) { require(exchange_byte(m_peer, value, true, true), "send native test command"); }
    void expect(char expected)
    {
        char actual = 0;
        require(exchange_byte(m_peer, actual, false, true) && actual == expected, "exact native test response");
    }

private:
    std::string     m_name;
    Native_endpoint m_listener = k_invalid_endpoint;
    Native_endpoint m_peer = k_invalid_endpoint;
};

int child_loop(Native_endpoint peer, const std::string& exec_path, char initial)
{
    if (!exchange_byte(peer, initial, true, false)) return 2;
    char command = 0;
    while (exchange_byte(peer, command, false, false)) {
        if (command == 'Q') {
            close_endpoint(peer);
            return 0;
        }
#if defined(__linux__)
        if (command == 'E') {
            if (fcntl(peer, F_SETFD, 0) != 0) return 3;
            const auto descriptor = std::to_string(peer);
            char* arguments[]{const_cast<char*>(exec_path.c_str()),
                const_cast<char*>("--native-peer-after-exec"), const_cast<char*>(descriptor.c_str()), nullptr};
            execv(exec_path.c_str(), arguments);
            return 4;
        }
#else
        (void)exec_path;
#endif
    }
    close_endpoint(peer);
    return 5;
}

sintra::Managed_child_occurrence_identity wait_native_ready(const sintra::Managed_child_custody& custody)
{
    sintra::Managed_child_native_change_signal changed;
    require(custody.observe_native_changes(changed), "original custody native observation");
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        const auto generation = changed.generation();
        const auto snapshot = custody.native_snapshot();
        if (snapshot.size() == 1 && snapshot.front().state == sintra::Managed_child_native_state::RUNNING &&
            snapshot.front().ownership_ready)
        {
            return snapshot.front().occurrence;
        }
        changed.wait_for_change(generation, deadline);
    }
    throw std::runtime_error("Original native ownership did not become ready");
}

int inspection_error()
{
#ifdef _WIN32
    return static_cast<int>(0xc0000022u); // STATUS_ACCESS_DENIED
#else
    return EACCES;
#endif
}

sintra::Managed_child_custody launch(
    const std::filesystem::path& image,
    Endpoint&                   endpoint,
    const std::filesystem::path& exec_image)
{
    // This earlier connection only coordinates test startup. It is never passed
    // to verify_native_peer: its birth cannot establish the proof precondition.
    Endpoint control;
    control.listen();
    sintra::Spawn_options options;
    options.binary_path = image.string();
    options.args = {k_child_flag, k_endpoint_flag, endpoint.name(),
        k_control_flag, control.name(), k_exec_path_flag, exec_image.string()};
    options.process_instance_id = k_child_iid;
    auto custody = sintra::spawn_swarm_process(options);
    require(static_cast<bool>(custody), "managed spawn retains original native custody");
    wait_native_ready(custody);
    // Only the name existed before launch. No listener or accepted proof
    // connection can predate this selected child's original native custody.
    endpoint.listen();
    control.accept_peer();
    control.expect('R');
    control.send('G');
    return custody;
}

void require_native_facts(const sintra::Managed_child_native_peer_proof& proof)
{
    if (proof.state != State::MATCH) {
        require(proof.native_process_id == 0 && proof.native_process_creation_identity == 0,
            "nonmatching proof carries no native authority identity");
        return;
    }
    require(proof.native_process_id != 0 && proof.native_process_creation_identity != 0,
        "matching proof supplies native process and creation identity");
    const auto stamp = sintra::query_process_start_stamp((uint32_t)proof.native_process_id);
    require(stamp && *stamp == proof.native_process_creation_identity,
        "matching creation identity equals the independent live native query");
}

int run_root(int argc, char** argv, const std::filesystem::path& directory)
{
    sintra::init(argc, argv);
    try {
        const auto original = std::filesystem::absolute(sintra::test::get_binary_path(argc, argv));
        const auto selected_path = directory / "selected_child.exe";
        const auto other_path = directory / "other_child.exe";
        const auto renamed_path = directory / "renamed_child.exe";
        std::filesystem::copy_file(original, selected_path);
        std::filesystem::copy_file(original, other_path);
        auto selected = sintra::capture_managed_child_executable(selected_path.string());
        auto other = sintra::capture_managed_child_executable(other_path.string());
        require(selected.reference && other.reference, "capture executable file objects before launch");
        require(!sintra::capture_managed_child_executable((directory / "absent.exe").string()).reference,
            "missing selected file is unavailable");
        Endpoint endpoint;
        auto custody = launch(selected_path, endpoint, other_path);
        const auto identity = wait_native_ready(custody);
        endpoint.accept_peer();
        endpoint.expect('R');
        auto prove = [&](const auto& occurrence, const auto& executable) {
            const auto proof = custody.verify_native_peer(occurrence, endpoint.borrowed(), executable);
            require_native_facts(proof);
            return proof;
        };
        const auto original_proof = prove(identity, selected.reference);
        require(original_proof.state == State::MATCH, "actual authenticated original peer and image match");
        require(original_proof.native_process_id == (uint64_t)custody.native_snapshot().front().pid,
            "matching native PID belongs to the exact selected custody");
        require(prove(identity, other.reference).state == State::MISMATCH, "identical bytes in a different file do not match");
        auto stale = identity;
        ++stale.custody_identity;
        require(prove(stale, selected.reference).state == State::MISMATCH, "wrong custody token cannot match");
        stale = identity;
        ++stale.occurrence;
        require(prove(stale, selected.reference).state == State::MISMATCH, "wrong occurrence cannot match");
        require(prove(identity, sintra::Managed_child_executable_reference{}).state == State::UNAVAILABLE,
            "missing selected image reference is unavailable");
        const auto invalid = custody.verify_native_peer(identity, static_cast<uintptr_t>(-1), selected.reference);
        require_native_facts(invalid);
        require(invalid.state == State::UNAVAILABLE,
            "invalid connection is unavailable, not death");
        {
            Endpoint wrong_peer;
            wrong_peer.listen();
            const auto impostor = connect_peer(wrong_peer.name());
            require(impostor != k_invalid_endpoint, "create actual wrong-process connection");
            wrong_peer.accept_peer();
            const auto proof = custody.verify_native_peer(identity, wrong_peer.borrowed(), selected.reference);
            require_native_facts(proof);
            close_endpoint(impostor);
            require(proof.state == State::MISMATCH, "OS-authenticated different peer cannot claim child");
        }
        sintra::detail::test_hooks::s_native_peer_inspection_error.store(&inspection_error);
        const auto unreadable = prove(identity, selected.reference);
        sintra::detail::test_hooks::s_native_peer_inspection_error.store(nullptr);
        require(unreadable.state == State::UNAVAILABLE && unreadable.error.code == inspection_error(),
            "native inspection denial remains unavailable with exact error");
#ifdef _WIN32
        require(sintra::detail::native_image_mapping_state((LONG)0xc0000003u) == State::UNAVAILABLE,
            "unsupported native class never becomes mismatch or death");
        require(sintra::detail::native_image_mapping_state((LONG)0xc0123456u) == State::UNAVAILABLE,
            "unknown NT error never becomes mismatch or death");
#endif
        require(prove(identity, selected.reference).state == State::MATCH, "inspection failure changes no native custody");
        std::filesystem::rename(selected_path, renamed_path);
        std::filesystem::copy_file(other_path, selected_path);
        auto replacement = sintra::capture_managed_child_executable(selected_path.string());
        require(static_cast<bool>(replacement.reference), "capture replacement as distinct file object");
        require(prove(identity, selected.reference).state == State::MATCH, "renamed original loaded image still matches");
        require(prove(identity, replacement.reference).state == State::MISMATCH, "replacement at original pathname cannot match");
#if defined(__linux__)
        endpoint.send('E');
        endpoint.expect('E');
        require(prove(identity, selected.reference).state == State::MISMATCH, "same native process after exec is wrong image");
        const auto after_exec = prove(identity, other.reference);
        require(after_exec.state == State::MATCH, "exec image comparison uses actual loaded file");
        require(after_exec.native_process_id == original_proof.native_process_id &&
            after_exec.native_process_creation_identity == original_proof.native_process_creation_identity,
            "exec preserves the original native PID and creation identity");
#endif
        endpoint.send('Q');
        const auto settled = custody.terminate_until(Clock::now() + 10s);
        require(settled.release_state == sintra::Managed_child_release_state::complete, "sole original cleanup settles child");
        require(prove(identity, selected.reference).state == State::EXITED, "exact native exit is explicit");
        Endpoint successor_endpoint;
        auto successor = launch(selected_path, successor_endpoint, other_path);
        const auto successor_identity = wait_native_ready(successor);
        successor_endpoint.accept_peer();
        successor_endpoint.expect('R');
        auto prove_successor = [&](const auto& occurrence, const auto& executable) {
            const auto proof = successor.verify_native_peer(occurrence, successor_endpoint.borrowed(), executable);
            require_native_facts(proof);
            return proof;
        };
        require(prove_successor(identity, replacement.reference).state == State::MISMATCH,
            "reused managed IID cannot revive the retired custody token");
        require(prove_successor(successor_identity, selected.reference).state == State::MISMATCH,
            "path replacement before launch cannot retarget original selection");
        require(prove_successor(successor_identity, replacement.reference).state == State::MATCH,
            "new exact custody matches its preselected executable object");
        successor_endpoint.send('Q');
        require(successor.terminate_until(Clock::now() + 10s).release_state == sintra::Managed_child_release_state::complete,
            "successor original cleanup settles once");
        require(sintra::shutdown(), "runtime shutdown after exact native settlement");
        std::puts("native_peer_contract_passed=true");
        return 0;
    }
    catch (const std::exception& error) {
        sintra::detail::test_hooks::s_native_peer_inspection_error.store(nullptr);
        std::fprintf(stderr, "native peer contract failure: %s\n", error.what());
        sintra::shutdown();
        return 1;
    }
}

} // namespace

int main(int argc, char** argv)
{
#if defined(__linux__)
    if (argc == 3 && std::string(argv[1]) == "--native-peer-after-exec") {
        return child_loop(std::stoi(argv[2]), {}, 'E');
    }
#endif
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        const auto control = connect_peer(sintra::test::get_argv_value(argc, argv, k_control_flag));
        if (control == k_invalid_endpoint) {
            return 2;
        }
        char command = 'R';
        const bool released = exchange_byte(control, command, true, false) &&
            exchange_byte(control, command, false, false) && command == 'G';
        close_endpoint(control);
        if (!released) {
            return 2;
        }
        const auto peer = connect_peer(sintra::test::get_argv_value(argc, argv, k_endpoint_flag));
        if (peer == k_invalid_endpoint) return 2;
        return child_loop(peer, sintra::test::get_argv_value(argc, argv, k_exec_path_flag), 'R');
    }
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "native_peer_identity");
    return run_root(argc, argv, std::filesystem::absolute(shared.path()));
}

#else

#include "test_utils.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char* argv[])
{
    const auto binary = std::filesystem::absolute(sintra::test::get_binary_path(argc, argv));
    const auto capture = sintra::capture_managed_child_executable(binary.string());
    if (capture.reference || capture.error.domain != sintra::Managed_child_native_error_domain::PROVIDER ||
        capture.error.operation.empty() ||
        sintra::compare_managed_child_executables(capture.reference, capture.reference))
    {
        std::fprintf(stderr, "FAILED: unavailable native executable identity must report a provider diagnostic "
            "without a reference or an equality claim: reference=%d domain=%u operation=%s\n",
            static_cast<bool>(capture.reference), static_cast<unsigned>(capture.error.domain),
            capture.error.operation.c_str());
        return 1;
    }
    return 0;
}

#endif
