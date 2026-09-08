// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using sintra::Managed_child_native_admission;
using sintra::Managed_child_native_error_domain;
using sintra::Managed_child_native_outcome;
using sintra::Managed_child_native_state;

constexpr const char* k_child_flag  = "--native-elevation-child";
constexpr const char* k_broker_flag = "--native-elevation-broker-event";
constexpr const char* k_ready_name  = "native_elevation_child_ready";

struct Setup_gate
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered = false;
    bool                    released = false;
};

Setup_gate* s_setup_gate = nullptr;

bool hold_native_setup(const char* stage, sintra::instance_id_type, uint32_t) noexcept
{
    if (std::string_view(stage) == sintra::detail::test_hooks::k_managed_child_fail_post_native_setup && s_setup_gate) {
        std::unique_lock<std::mutex> lock(s_setup_gate->mutex);
        s_setup_gate->entered = true;
        s_setup_gate->changed.notify_all();
        s_setup_gate->changed.wait_for(lock, 5s, []() { return s_setup_gate->released; });
    }
    return false;
}


bool check(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return value;
}

int run_child(int argc, char* argv[])
{
    sintra::init(argc, argv);
    sintra::Transceiver ready;
    if (!ready.assign_name(k_ready_name)) {
        return 2;
    }
    std::this_thread::sleep_for(30s);
    std::_Exit(3);
}

#ifdef _WIN32

bool fail_elevation_observer(const char* stage, sintra::instance_id_type, uint32_t) noexcept
{
    return std::string_view(stage) == sintra::detail::test_hooks::k_managed_child_elevation_observer_start;
}

class Owned_broker
{
public:
    ~Owned_broker()
    {
        release();
        if (m_process) {
            WaitForSingleObject(m_process, 12000);
            CloseHandle(m_process);
        }
        if (m_event) {
            CloseHandle(m_event);
        }
    }

    bool launch(const std::string& binary)
    {
        SECURITY_ATTRIBUTES security = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        m_event = CreateEventW(&security, TRUE, FALSE, nullptr);
        if (!m_event) {
            return false;
        }
        const auto path = std::filesystem::path(binary).wstring();
        auto command = L"\"" + path + L"\" --native-elevation-broker-event " +
            std::to_wstring(reinterpret_cast<uintptr_t>(m_event));
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process))
        {
            return false;
        }
        m_process = process.hProcess;
        CloseHandle(process.hThread);
        return true;
    }

    HANDLE process() const { return m_process; }
    HANDLE transferred_process() const { return m_transferred_process; }

    HANDLE transfer_process()
    {
        // Retain a separate oracle handle; the originally launched object
        // handle itself transfers to the provider, without caller close.
        HANDLE observer = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), m_process, GetCurrentProcess(), &observer,
            0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            return nullptr;
        }
        m_transferred_process = std::exchange(m_process, observer);
        return m_transferred_process;
    }

    void release()
    {
        if (m_event) {
            SetEvent(m_event);
        }
    }

    bool wait_for_exit()
    {
        return WaitForSingleObject(m_process, 3000) == WAIT_OBJECT_0;
    }

private:
    HANDLE m_event = nullptr;
    HANDLE m_process = nullptr;
    HANDLE m_transferred_process = nullptr;
};

bool adopt_owned_broker(sintra::Managed_child_native_elevation_ticket& ticket, Owned_broker& broker)
{
    HANDLE transferred = broker.transfer_process();
    if (!transferred) {
        return false;
    }
    if (!ticket.adopt_broker(reinterpret_cast<uintptr_t>(transferred))) {
        CloseHandle(transferred);
        return false;
    }
    return ticket.last_error().domain == Managed_child_native_error_domain::NONE;
}

bool wait_for_failed_action(
    const sintra::Managed_child_custody& custody,
    const sintra::Managed_child_native_change_signal& changes)
{
    const auto deadline = Clock::now() + 3s;
    while (Clock::now() < deadline) {
        const auto generation = changes.generation();
        const auto snapshot = custody.native_snapshot();
        if (snapshot.size() == 1 && snapshot.front().action.outcome == Managed_child_native_outcome::FAILED) {
            return true;
        }
        changes.wait_for_change(generation, deadline);
    }
    return false;
}

bool exercise_ticket(
    const sintra::Managed_child_custody& custody,
    const sintra::Managed_child_occurrence_identity& identity,
    const sintra::Managed_child_native_change_signal& changes,
    const std::string& binary)
{
    bool valid = true;
    auto wrong = identity;
    ++wrong.occurrence;
    valid &= check(custody.request_native_elevation(wrong, Clock::now() + 30s).request.admission ==
        Managed_child_native_admission::REJECTED, "unknown exact occurrence cannot reserve elevation");
    valid &= check(custody.request_native_elevation(identity, Clock::now()).request.admission ==
        Managed_child_native_admission::REJECTED, "expired consent reservation is rejected");

    sintra::detail::test_hooks::s_managed_child_failure.store(&fail_elevation_observer);
    const auto observer_failure = custody.request_native_elevation(identity, Clock::now() + 30s);
    sintra::detail::test_hooks::s_managed_child_failure.store(nullptr);
    valid &= check(!observer_failure.ticket && observer_failure.request.admission ==
        Managed_child_native_admission::REJECTED &&
        custody.native_snapshot().front().action.outcome == Managed_child_native_outcome::FAILED,
        "observer startup failure rejects before any ticket can launch a broker");

    auto cancelled = custody.request_native_elevation(identity, Clock::now() + 30s);
    valid &= check(cancelled.ticket && cancelled.request.admission == Managed_child_native_admission::STARTED,
        "exact original custody reserves elevation");
    if (!cancelled.ticket) {
        return false;
    }
    const auto first_generation = cancelled.ticket->action_generation();
    valid &= check(cancelled.ticket->occurrence() == identity, "ticket retains complete occurrence identity");
    valid &= check(cancelled.ticket->export_reference().reference_handle == 0,
        "reference is unavailable before exact broker binding");
    valid &= check(custody.request_native_termination(identity, Clock::now() + 5s).admission ==
        Managed_child_native_admission::ALREADY_ACTIVE, "normal retry shares elevation serializer");
    cancelled.ticket.reset();
    valid &= check(custody.native_snapshot().front().action.outcome == Managed_child_native_outcome::CANCELLED,
        "unbound abandoned consent settles without native termination");

    auto pending = custody.request_native_elevation(identity, Clock::now() + 30s);
    if (!check((bool)pending.ticket, "new explicit consent obtains newer reservation")) {
        return false;
    }
    valid &= check(pending.ticket->action_generation() > first_generation, "new consent has newer action generation");
    valid &= check(!pending.ticket->bind_broker(0) &&
        pending.ticket->last_error().domain == Managed_child_native_error_domain::WINDOWS &&
        pending.ticket->last_error().code == ERROR_INVALID_HANDLE,
        "invalid broker handle retains exact Windows duplication error");
    Owned_broker held_broker;
    if (!check(held_broker.launch(binary), "owned held broker starts")) {
        return false;
    }
    valid &= check(adopt_owned_broker(*pending.ticket, held_broker),
        "provider adopts exact originally launched broker handle");
    HANDLE rejected_transfer = held_broker.transfer_process();
    valid &= check(rejected_transfer && !pending.ticket->adopt_broker(reinterpret_cast<uintptr_t>(rejected_transfer)) &&
        GetProcessId(rejected_transfer) == GetProcessId(held_broker.process()),
        "rejected repeated adoption leaves the caller as sole handle owner");
    if (rejected_transfer) {
        valid &= check(CloseHandle(rejected_transfer) != FALSE, "caller closes rejected transfer exactly once");
    }
    const auto reference = pending.ticket->export_reference();
    valid &= check(reference.source_pid == GetCurrentProcessId() && reference.reference_handle != 0 &&
        reference.target_pid == (uint32_t)custody.native_snapshot().front().pid,
        "export carries scoped exact reference and diagnostic locators");
    if (reference.reference_handle == 0) {
        return false;
    }
    HANDLE retained = reinterpret_cast<HANDLE>(reference.reference_handle);
    const BOOL reference_terminated = TerminateProcess(retained, 9);
    const DWORD reference_error = reference_terminated ? ERROR_SUCCESS : GetLastError();
    valid &= check(!reference_terminated && reference_error == ERROR_ACCESS_DENIED &&
        WaitForSingleObject(retained, 0) == WAIT_TIMEOUT,
        "reduced exact reference cannot terminate the still-live target");
    valid &= check(!pending.ticket->finish(Managed_child_native_outcome::CANCELLED),
        "even pre-grant completion cannot release a still-live bound broker");
    pending.ticket.reset();
    valid &= check(custody.request_native_termination(identity, Clock::now() + 5s).admission ==
        Managed_child_native_admission::ALREADY_ACTIVE,
        "controller loss cannot admit conflicting retry while broker survives");
    held_broker.release();
    valid &= check(held_broker.wait_for_exit() && wait_for_failed_action(custody, changes),
        "exact broker exit wakes inventory and settles abandoned action");
    valid &= check(custody.native_snapshot().front().state == Managed_child_native_state::RUNNING,
        "broker exit is distinct from native child exit");

    auto abandoned_grant = custody.request_native_elevation(identity, Clock::now() + 30s);
    Owned_broker abandoned_broker;
    if (!check(abandoned_grant.ticket && abandoned_broker.launch(binary), "owned post-grant loss fixture starts")) {
        return false;
    }
    valid &= check(abandoned_grant.ticket->bind_broker(reinterpret_cast<uintptr_t>(abandoned_broker.process())) &&
        abandoned_grant.ticket->commit(), "post-grant loss fixture obtains one exact grant");
    abandoned_grant.ticket.reset();
    valid &= check(custody.request_native_termination(identity, Clock::now() + 5s).admission ==
        Managed_child_native_admission::ALREADY_ACTIVE, "post-grant controller loss retains original action");
    abandoned_broker.release();
    valid &= check(abandoned_broker.wait_for_exit() && wait_for_failed_action(custody, changes),
        "post-grant abandoned action settles only after exact broker exit");

    const auto consent_deadline = Clock::now() + 30ms;
    auto late_consent = custody.request_native_elevation(identity, consent_deadline);
    Owned_broker late_broker;
    if (!check(late_consent.ticket && late_broker.launch(binary), "owned late-consent fixture starts")) {
        return false;
    }
    std::this_thread::sleep_until(consent_deadline + 10ms);
    valid &= check(adopt_owned_broker(*late_consent.ticket, late_broker),
        "expired consent still accounts for its exact late-launched helper");
    valid &= check(!late_consent.ticket->commit() && late_consent.ticket->export_reference().reference_handle == 0,
        "expired original consent grants no native authority");
    late_broker.release();
    valid &= check(late_broker.wait_for_exit() && late_consent.ticket->finish(Managed_child_native_outcome::CANCELLED),
        "late helper settles after exact exit");

    auto early_death = custody.request_native_elevation(identity, Clock::now() + 30s);
    Owned_broker exited_broker;
    if (!check(early_death.ticket && exited_broker.launch(binary), "owned early-death fixture starts")) {
        return false;
    }
    exited_broker.release();
    valid &= check(exited_broker.wait_for_exit() &&
        adopt_owned_broker(*early_death.ticket, exited_broker),
        "broker death before binding is retained as exact native fact");
    valid &= check(!early_death.ticket->commit() && early_death.ticket->finish(Managed_child_native_outcome::FAILED),
        "already exited broker receives no grant and action remains settleable");

    auto granted = custody.request_native_elevation(identity, Clock::now() + 30s);
    if (!check((bool)granted.ticket, "retry available after original broker exit")) {
        return false;
    }
    Owned_broker broker;
    if (!check(broker.launch(binary), "owned grant broker starts")) {
        return false;
    }
    valid &= check(adopt_owned_broker(*granted.ticket, broker),
        "new action binds its own exact broker");
    const auto grant_reference = granted.ticket->export_reference();
    HANDLE candidate = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, grant_reference.target_pid);
    using Compare_objects = BOOL (WINAPI*)(HANDLE, HANDLE);
    const auto compare = reinterpret_cast<Compare_objects>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles"));
    valid &= check(candidate && compare, "exact-object comparison and independently acquired candidate are available");
    if (!candidate || !compare) {
        if (candidate) {
            CloseHandle(candidate);
        }
        return false;
    }
    const bool exact = compare(candidate, reinterpret_cast<HANDLE>(grant_reference.reference_handle)) != FALSE;
    valid &= check(exact && !compare(broker.process(), reinterpret_cast<HANDLE>(grant_reference.reference_handle)),
        "candidate must name reference object; unrelated broker object is rejected");
    const bool committed = granted.ticket->commit();
    valid &= check(committed && !granted.ticket->commit(), "exact live action grants authority once only");
    valid &= check(custody.request_native_termination(identity, Clock::now() + 5s).admission ==
        Managed_child_native_admission::ALREADY_ACTIVE, "granted action excludes ordinary native retry");
    const bool terminated = exact && committed && TerminateProcess(candidate, 9);
    valid &= check(terminated && WaitForSingleObject(candidate, 3000) == WAIT_OBJECT_0,
        "rights-bearing compared handle terminates only owned exact target");
    CloseHandle(candidate);
    broker.release();
    valid &= check(broker.wait_for_exit(), "exact granted broker exits before completion");
    valid &= check(GetProcessId(broker.transferred_process()) == GetProcessId(broker.process()) &&
        WaitForSingleObject(broker.transferred_process(), 0) == WAIT_OBJECT_0,
        "controller borrow remains valid after provider observes adopted broker exit");
    valid &= check(granted.ticket->finish(Managed_child_native_outcome::EXITED),
        "completion directly rechecks exact native exit before settling");
    valid &= check(custody.native_snapshot().front().state == Managed_child_native_state::EXITED &&
        custody.native_snapshot().front().action.outcome == Managed_child_native_outcome::EXITED,
        "original custody publishes native exit from elevated authority");
    valid &= check(GetProcessId(broker.transferred_process()) == GetProcessId(broker.process()),
        "settling action does not close handle still borrowed through live ticket");
    return valid;
}

#endif

int run_root(int argc, char* argv[])
{
    sintra::init(argc, argv);
    sintra::Spawn_options options;
    options.binary_path = sintra::test::get_binary_path(argc, argv);
    options.args = {k_child_flag};
    options.process_instance_id = sintra::compose_instance(48u, 1ull);
    options.readiness_instance_name = k_ready_name;
    options.lifetime.enable_lifeline = false;
    Setup_gate setup;
    s_setup_gate = &setup;
    sintra::detail::test_hooks::s_managed_child_failure.store(&hold_native_setup);
    auto custody = sintra::spawn_swarm_process(options);
    sintra::Managed_child_native_change_signal changes;
    custody.observe_native_changes(changes);
    bool valid = true;
    {
        std::unique_lock<std::mutex> lock(setup.mutex);
        valid &= check(setup.changed.wait_for(lock, 3s, [&]() { return setup.entered; }),
            "native-created setup barrier reached");
    }
    const auto pending = custody.native_snapshot();
    valid &= check(pending.size() == 1 && pending.front().state == Managed_child_native_state::RUNNING &&
        !pending.front().ownership_ready, "native existence is distinct from completed custody ownership transfer");
    if (pending.size() == 1) {
        valid &= check(custody.request_native_termination(pending.front().occurrence, Clock::now() + 5s).admission ==
            Managed_child_native_admission::REJECTED, "held setup cannot admit native action before ownership transfer");
    }
    const auto before_settlement = changes.generation();
    {
        std::lock_guard<std::mutex> lock(setup.mutex);
        setup.released = true;
    }
    setup.changed.notify_all();
    const auto ownership_deadline = Clock::now() + 3s;
    while (Clock::now() < ownership_deadline) {
        const auto generation = changes.generation();
        const auto current = custody.native_snapshot();
        if (current.size() == 1 && current.front().ownership_ready) {
            break;
        }
        changes.wait_for_change(generation, ownership_deadline);
    }
    sintra::detail::test_hooks::s_managed_child_failure.store(nullptr);
    s_setup_gate = nullptr;
    valid &= check(custody.native_snapshot().size() == 1 && custody.native_snapshot().front().ownership_ready &&
        changes.generation() != before_settlement, "setup settlement publishes ownership readiness and wakes native fan-in");
    valid &= check(custody.wait_for_readiness_until(Clock::now() + 8s).readiness_state ==
        sintra::Managed_child_readiness_state::reached, "owned native child readiness");
    const auto snapshot = custody.native_snapshot();
    if (snapshot.size() == 1) {
        valid &= check(snapshot.front().ownership_ready, "native ownership transfer completed before application readiness");
        const auto identity = snapshot.front().occurrence;
#ifdef _WIN32
        valid &= exercise_ticket(custody, identity, changes, options.binary_path);
#else
        const auto unsupported = custody.request_native_elevation(identity, Clock::now() + 30s);
        valid &= check(!unsupported.ticket && unsupported.request.admission == Managed_child_native_admission::REJECTED &&
            unsupported.request.error.domain == Managed_child_native_error_domain::PROVIDER,
            "unsupported platform truthfully rejects elevation without native action");
#endif
        custody.request_native_termination(identity, Clock::now() + 5s);
    }
    else {
        valid = check(false, "one exact native occurrence exists");
    }
    valid &= check(custody.terminate_until(Clock::now() + 12s).release_state ==
        sintra::Managed_child_release_state::complete, "original custody cleanup completes");
    valid &= check(sintra::shutdown(), "runtime joins native observers and shuts down");
    return valid ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef _WIN32
    const auto event = sintra::test::get_argv_value(argc, argv, k_broker_flag);
    if (!event.empty()) {
        HANDLE release = reinterpret_cast<HANDLE>((uintptr_t)std::stoull(event));
        const DWORD wait = WaitForSingleObject(release, 10000);
        CloseHandle(release);
        return wait == WAIT_OBJECT_0 ? 0 : 3;
    }
#endif
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "mc_native_elevation");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv);
    }
    return run_root(argc, argv);
}
