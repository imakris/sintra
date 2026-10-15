// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>
#include "test_utils.h"

#include <cstdio>

#if defined(__APPLE__)
#include <Security/Security.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>

extern char** environ;

namespace {

using State = sintra::Managed_child_native_peer_state;

std::filesystem::path s_selected_during_capture;
std::filesystem::path s_replacement_during_capture;
std::filesystem::path s_retired_during_capture;
int s_capture_hook_calls = 0;

void replace_selected_during_capture()
{
    ++s_capture_hook_calls;
    std::filesystem::rename(s_selected_during_capture, s_retired_during_capture);
    std::filesystem::rename(s_replacement_during_capture, s_selected_during_capture);
}

void require(bool passed, const char* contract)
{
    if (!passed) {
        std::fprintf(stderr, "FAILED: %s\n", contract);
        throw std::runtime_error(contract);
    }
}

void require_capture_error(
    const sintra::Managed_child_executable_capture& capture,
    OSStatus code, const char* operation, const char* contract)
{
    if (capture.reference || capture.error.domain != sintra::Managed_child_native_error_domain::PROVIDER ||
        capture.error.code != code || capture.error.operation != operation)
    {
        std::fprintf(stderr, "capture result: reference=%d code=%d operation=%s\n",
            (bool)capture.reference, capture.error.code, capture.error.operation.c_str());
        require(false, contract);
    }
}

void sign_code(const std::filesystem::path& code, const char* identifier)
{
    std::string path = code.string();
    char* arguments[]{const_cast<char*>("/usr/bin/codesign"), const_cast<char*>("--force"),
        const_cast<char*>("--sign"), const_cast<char*>("-"), const_cast<char*>("--identifier"),
        const_cast<char*>(identifier), path.data(), nullptr};
    pid_t signer = -1;
    require(posix_spawn(&signer, arguments[0], nullptr, nullptr, arguments, environ) == 0,
        "launch system signer for isolated fixture");
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(signer, &status, 0);
    }
    while (waited < 0 && errno == EINTR);
    require(waited == signer && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "sign complete isolated fixture");
}

bool transfer(int descriptor, void* data, size_t size, bool sending)
{
    auto* bytes = static_cast<char*>(data);
    while (size != 0) {
        pollfd ready{descriptor, (short)(sending ? POLLOUT : POLLIN), 0};
        if (poll(&ready, 1, 5000) <= 0) {
            return false;
        }
        const auto count = sending ? write(descriptor, bytes, size) : read(descriptor, bytes, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size  -= (size_t)count;
    }
    return true;
}

bool wait_child(pid_t pid)
{
    int status = 0;
    pid_t observed;
    do {
        observed = waitpid(pid, &status, 0);
    }
    while (observed < 0 && errno == EINTR);
    return observed == pid;
}

int native_owner(int control, const std::string& executable)
{
    int ready[2];
    if (pipe(ready) != 0) {
        return 2;
    }
    const auto descriptor = std::to_string(ready[1]);
    const auto child = fork();
    if (child == 0) {
        close(control);
        close(ready[0]);
        execl(executable.c_str(), executable.c_str(), "--auth-workload", descriptor.c_str(), nullptr);
        _exit(127);
    }
    close(ready[1]);
    if (child < 0) {
        close(ready[0]);
        return 2;
    }
    char byte = 0;
    const bool started = transfer(ready[0], &byte, 1, false) && byte == 'R';
    close(ready[0]);
    auto pid = child;
    bool ok = started && transfer(control, &pid, sizeof(pid), true) && transfer(control, &byte, 1, false);
    kill(child, SIGKILL);
    ok = wait_child(child) && ok;
    byte = 'E';
    ok = transfer(control, &byte, 1, true) && ok;
    close(control);
    return ok ? 0 : 2;
}

class Nonparent_workload
{
public:
    explicit Nonparent_workload(const std::string& executable)
    {
        int endpoints[2];
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints) == 0, "create native-owner channel");
        m_owner = fork();
        if (m_owner == 0) {
            close(endpoints[0]);
            _exit(native_owner(endpoints[1], executable));
        }
        close(endpoints[1]);
        m_control = endpoints[0];
        try {
            require(m_owner > 0, "fork sole native owner");
            require(transfer(m_control, &m_pid, sizeof(m_pid), false), "receive original live grandchild");
            const auto stamp = sintra::query_process_start_stamp((uint32_t)m_pid);
            require(stamp.has_value(), "capture original grandchild public birth identity");
            m_birth = *stamp;
        }
        catch (...) {
            close(m_control);
            if (m_owner > 0) wait_child(m_owner);
            throw;
        }
    }

    ~Nonparent_workload()
    {
        stop();
        if (m_control >= 0) close(m_control);
        if (m_owner > 0) wait_child(m_owner);
    }

    bool stop()
    {
        if (m_stopped) {
            return true;
        }
        m_stopped = true;
        char byte = 'K';
        return transfer(m_control, &byte, 1, true) && transfer(m_control, &byte, 1, false) && byte == 'E';
    }

    pid_t pid() const { return m_pid; }
    uint64_t birth() const { return m_birth; }

private:
    pid_t m_owner = -1;
    pid_t m_pid = -1;
    int m_control = -1;
    uint64_t m_birth = 0;
    bool m_stopped = false;
};

class Retained_observer
{
public:
    explicit Retained_observer(const Nonparent_workload& workload)
        : m_pid(workload.pid()), m_birth(workload.birth()), m_queue(kqueue())
    {
        try {
            require(m_queue >= 0 && stamp_matches(), "birth observed before local observer registration");
            struct kevent change{};
            EV_SET(&change, m_pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_EXIT, 0, nullptr);
            require(kevent(m_queue, &change, 1, nullptr, 0, nullptr) == 0 && stamp_matches(),
                "local NOTE_EXIT observer bracketed by original birth observations");
        }
        catch (...) {
            if (m_queue >= 0) close(m_queue);
            throw;
        }
    }

    ~Retained_observer() { if (m_queue >= 0) close(m_queue); }

    State observe()
    {
        if (m_state != State::MATCH) {
            return m_state;
        }
        struct kevent event{};
        timespec now{};
        int count;
        do {
            count = kevent(m_queue, nullptr, 0, &event, 1, &now);
        }
        while (count < 0 && errno == EINTR);
        if (count < 0 || (count > 0 && (event.flags & EV_ERROR) != 0)) {
            m_state = State::UNAVAILABLE;
        }
        else
        if (count > 0) {
            m_state = event.filter == EVFILT_PROC && event.ident == (uintptr_t)m_pid &&
                (event.fflags & NOTE_EXIT) != 0 ? State::EXITED : State::UNAVAILABLE;
        }
        else
        if (!stamp_matches()) {
            m_state = State::UNAVAILABLE;
        }
        return m_state;
    }

    void fail_descriptor()
    {
        close(m_queue);
        m_queue = -1;
    }

private:
    bool stamp_matches() const
    {
        const auto stamp = sintra::query_process_start_stamp((uint32_t)m_pid);
        return stamp && *stamp == m_birth;
    }

    pid_t m_pid;
    uint64_t m_birth;
    int m_queue;
    State m_state = State::MATCH;
};

void exercise_direct(
    const sintra::Managed_child_executable_reference& selected,
    const sintra::Managed_child_executable_reference& different)
{
    // fork retains this signed image while giving this fixture sole wait authority.
    const auto child = fork();
    if (child == 0) {
        alarm(12);
        for (;;) pause();
    }
    require(child > 0, "fork direct child with selected signed CODE");
    try {
        const auto stamp = sintra::query_process_start_stamp((uint32_t)child);
        require(stamp.has_value(), "capture direct child birth identity");
        const auto matched = sintra::verify_process_executable(child, selected);
        require(matched.state == State::MATCH && matched.native_process_id == (uint64_t)child &&
            matched.native_process_creation_identity == *stamp, "direct child matches original CODE and occurrence");
        const auto mismatch = sintra::verify_process_executable(child, different);
        require(mismatch.state == State::MISMATCH && mismatch.error.code == errSecCSReqFailed &&
            mismatch.error.operation == "SecCodeCheckValidity(original occurrence, exact code requirement)",
            "direct child rejects a different valid CodeDirectory");
        require(kill(child, SIGKILL) == 0, "terminate original direct child");
        siginfo_t exit{};
        int observed;
        do {
            observed = waitid(P_PID, child, &exit, WEXITED | WNOWAIT);
        }
        while (observed < 0 && errno == EINTR);
        require(observed == 0 && exit.si_pid == child, "observe direct child exit without consuming it");
        const auto exited = sintra::verify_process_executable(child, selected);
        require(exited.state == State::EXITED && exited.native_process_id == 0,
            "direct child exit cannot authorize selected CODE");
    }
    catch (...) {
        kill(child, SIGKILL);
        wait_child(child);
        throw;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    }
    while (waited < 0 && errno == EINTR);
    require(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
        "verification preserves the direct child's exact consuming wait");
}

void exercise_observed(
    const std::string& executable,
    const sintra::Managed_child_executable_reference& selected,
    const sintra::Managed_child_executable_reference& different)
{
    {
        Nonparent_workload workload(executable);
        Retained_observer observer(workload);
        const auto observe = [&]() { return observer.observe(); };
        const auto proof = sintra::verify_observed_process_executable(workload.pid(), workload.birth(), selected, observe);
        require(proof.state == State::MATCH && proof.native_process_id == (uint64_t)workload.pid() &&
            proof.native_process_creation_identity == workload.birth(), "nonparent original CODE and occurrence match");
        const auto mismatch = sintra::verify_observed_process_executable(
            workload.pid(), workload.birth(), different, observe);
        require(mismatch.state == State::MISMATCH && mismatch.error.code == errSecCSReqFailed &&
            mismatch.error.operation == "SecCodeCheckValidity(original occurrence, exact code requirement)",
            "nonparent pid guest rejects a different valid CodeDirectory");
        require(sintra::verify_process_executable(workload.pid(), selected).state == State::UNAVAILABLE,
            "direct-child API cannot waitid a nonparent workload");
        const auto wrong_birth = sintra::verify_observed_process_executable(
            workload.pid(), workload.birth() + 1, selected, observe);
        require(wrong_birth.state != State::MATCH && wrong_birth.native_process_id == 0 &&
            wrong_birth.native_process_creation_identity == 0,
            "claimed different birth cannot authorize an original native occurrence");
        require(sintra::verify_observed_process_executable(workload.pid(), workload.birth(), selected, {}).state ==
            State::UNAVAILABLE, "missing retained observer cannot authorize code");
        require(workload.stop(), "sole owner reaps original grandchild");
        require(sintra::verify_observed_process_executable(workload.pid(), workload.birth(), selected, observe).state ==
            State::EXITED && observer.observe() == State::EXITED, "consumed NOTE_EXIT remains terminal");
    }
    {
        Nonparent_workload workload(executable);
        Retained_observer observer(workload);
        int observations = 0;
        // The observer brackets Security validation; the second call is the final observation.
        const auto proof = sintra::verify_observed_process_executable(workload.pid(), workload.birth(), selected, [&]() {
            if (++observations == 2) {
                require(workload.stop(), "reap original after dynamic validation before final observation");
            }
            return observer.observe();
        });
        require(observations == 2 && proof.state == State::EXITED && proof.native_process_id == 0,
            "original exit during authorization cannot produce a matching native proof");
    }
    {
        Nonparent_workload workload(executable);
        Retained_observer observer(workload);
        int observations = 0;
        const auto proof = sintra::verify_observed_process_executable(workload.pid(), workload.birth(), selected, [&]() {
            if (++observations == 2) observer.fail_descriptor();
            return observer.observe();
        });
        require(observations == 2 && proof.state == State::UNAVAILABLE && observer.observe() == State::UNAVAILABLE,
            "observer failure after valid CODE remains unavailable monotonically");
    }
}

std::streamoff sealed_text_offset(const std::filesystem::path& executable)
{
    std::ifstream bytes(executable, std::ios::binary);
    mach_header_64 header{};
    bytes.read(reinterpret_cast<char*>(&header), sizeof(header));
    require(bytes.good() && header.magic == MH_MAGIC_64, "fixture is a native thin Mach-O executable");
    for (uint32_t index = 0; index < header.ncmds; ++index) {
        const auto position = bytes.tellg();
        load_command command{};
        bytes.read(reinterpret_cast<char*>(&command), sizeof(command));
        require(bytes.good() && command.cmdsize >= sizeof(command), "read fixture Mach-O load command");
        if (command.cmd == LC_SEGMENT_64) {
            bytes.seekg(position);
            segment_command_64 segment{};
            bytes.read(reinterpret_cast<char*>(&segment), sizeof(segment));
            require(bytes.good(), "read fixture Mach-O segment");
            if (std::strncmp(segment.segname, "__TEXT", sizeof(segment.segname)) == 0) {
                for (uint32_t section_index = 0; section_index < segment.nsects; ++section_index) {
                    section_64 section{};
                    bytes.read(reinterpret_cast<char*>(&section), sizeof(section));
                    require(bytes.good(), "read fixture Mach-O text section");
                    if (std::strncmp(section.sectname, "__text", sizeof(section.sectname)) == 0) {
                        require(section.size > 0, "fixture has sealed instructions outside Mach-O headers");
                        return (std::streamoff)(section.offset + section.size / 2);
                    }
                }
            }
        }
        bytes.seekg(position + (std::streamoff)command.cmdsize);
    }
    throw std::runtime_error("fixture has no __TEXT,__text section");
}

void exercise_static(const std::filesystem::path& executable, const std::filesystem::path& directory)
{
    const auto modified = directory / "modified-code";
    std::filesystem::copy_file(executable, modified);
    require((bool)sintra::capture_managed_child_executable(modified.string()).reference,
        "unmodified capture of the copy succeeds before corrupting sealed instructions");
    const auto text_offset = sealed_text_offset(modified);
    {
        std::fstream bytes(modified, std::ios::in | std::ios::out | std::ios::binary);
        bytes.seekg(text_offset);
        char byte = 0;
        bytes.read(&byte, 1);
        require(bytes.good(), "read sealed native code byte");
        byte ^= 1;
        bytes.seekp(text_offset);
        bytes.write(&byte, 1);
        require(bytes.good(), "modify sealed native code byte");
    }
    require_capture_error(sintra::capture_managed_child_executable(modified.string()),
        errSecCSSignatureFailed, "SecStaticCodeCheckValidity(complete selected code)",
        "modified instructions fail signature validation rather than Mach-O parsing");

    s_selected_during_capture    = directory / "capture-selection";
    s_replacement_during_capture = directory / "capture-replacement";
    s_retired_during_capture     = directory / "capture-retired";
    std::filesystem::copy_file(executable, s_selected_during_capture);
    std::filesystem::copy_file(executable, s_replacement_during_capture);
    require((bool)sintra::capture_managed_child_executable(s_selected_during_capture.string()).reference,
        "original selection captures before replacement");
    require((bool)sintra::capture_managed_child_executable(s_replacement_during_capture.string()).reference,
        "replacement captures with the same valid signed CODE");
    s_capture_hook_calls = 0;
    sintra::detail::test_hooks::s_darwin_static_validation_hook.store(&replace_selected_during_capture);
    const auto changed = sintra::capture_managed_child_executable(s_selected_during_capture.string());
    sintra::detail::test_hooks::s_darwin_static_validation_hook.store(nullptr);
    require(s_capture_hook_calls == 1, "selection was replaced after exact requirement creation");
    require_capture_error(changed, errSecCSStaticCodeChanged,
        "Selected executable changed during static validation",
        "observed selection replacement during capture rejects even identical valid code");

    for (const auto* contents : {"#!/bin/sh\nexit 0\n", "ordinary data\n"}) {
        const auto non_code = directory / "non-code";
        std::ofstream(non_code) << contents;
        require_capture_error(sintra::capture_managed_child_executable(non_code.string()),
            errSecCSBadObjectFormat, "Selected executable must contain native Mach-O code",
            "scripts and data cannot bypass native Mach-O selection");
    }

    const auto bundle = directory / "Selected.app";
    const auto main = bundle / "Contents/MacOS/auth";
    const auto resource = bundle / "Contents/Resources/selection.txt";
    std::filesystem::create_directories(main.parent_path());
    std::filesystem::create_directories(resource.parent_path());
    std::filesystem::copy_file(executable, main);
    std::ofstream(bundle / "Contents/Info.plist") <<
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>"
        "<key>CFBundleExecutable</key><string>auth</string>"
        "<key>CFBundleIdentifier</key><string>org.sintra.auth-contract</string>"
        "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>";
    std::ofstream(resource) << "selected generation\n";
    sign_code(bundle, "org.sintra.auth-contract");
    const auto captured = sintra::capture_managed_child_executable(main.string());
    if (!captured.reference) {
        std::fprintf(stderr, "bundle capture: %d %s\n", captured.error.code, captured.error.operation.c_str());
    }
    require(static_cast<bool>(captured.reference), "canonical full bundle capture validates sealed resources");
    std::ofstream(resource) << "changed resource\n";
    require(sintra::compare_managed_child_executable_path(captured.reference, main.string()),
        "cheap selection comparison stays FILE-only and does not revalidate resources");
    require(!sintra::capture_managed_child_executable(main.string()).reference,
        "new capture rejects changed sealed resource in the complete bundle");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--auth-workload") == 0) {
        const int descriptor = std::stoi(argv[2]);
        char ready = 'R';
        if (!transfer(descriptor, &ready, 1, true)) return 2;
        close(descriptor);
        for (;;) pause();
    }
    signal(SIGPIPE, SIG_IGN);
    alarm(45);
    try {
        sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "darwin_auth_contract");
        const auto executable = std::filesystem::absolute(sintra::test::get_binary_path(argc, argv));
        const auto selected = sintra::capture_managed_child_executable(executable.string());
        if (!selected.reference) {
            std::fprintf(stderr, "standalone capture: %d %s\n", selected.error.code, selected.error.operation.c_str());
        }
        require(static_cast<bool>(selected.reference), "capture signed native standalone executable");
        const auto directory = std::filesystem::absolute(shared.path());
        const auto different_code = directory / "different-code";
        std::filesystem::copy_file(executable, different_code);
        sign_code(different_code, "org.sintra.auth-contract.different");
        const auto different = sintra::capture_managed_child_executable(different_code.string());
        require((bool)different.reference, "capture differently signed native executable");
        exercise_direct(selected.reference, different.reference);
        exercise_observed(executable.string(), selected.reference, different.reference);
        exercise_static(executable, directory);
        std::puts("darwin_executable_auth_contract_passed=true");
        return 0;
    }
    catch (const std::exception& error) {
        sintra::detail::test_hooks::s_darwin_static_validation_hook.store(nullptr);
        std::fprintf(stderr, "Darwin executable authentication contract failed: %s\n", error.what());
        return 1;
    }
}

#else
int main()
{
    std::puts("[SINTRA_DID_NOT_RUN] executable CODE authentication is Darwin-only");
    return 0;
}
#endif
