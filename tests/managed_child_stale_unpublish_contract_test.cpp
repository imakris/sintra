// Fail-first contract for exact managed-child publication retirement.
//
// A service request from occurrence N must not retire the publication that
// currently belongs to occurrence N+1, even when both use the same raw IID.

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::uint64_t k_custody_identity       = 0x5a71u;
constexpr std::uint32_t k_stale_occurrence       = 7;
constexpr std::uint32_t k_replacement_occurrence = 8;
constexpr std::uint64_t k_transceiver_index      = 0x6a51u;
constexpr std::string_view k_observer_flag       =
    "--managed_child_unpublish_order_observer";
constexpr std::string_view k_observer_name =
    "managed_child_unpublish_order_observer";
constexpr std::string_view k_observer_ack_name =
    "managed_child_unpublish_order_observer_ack";
constexpr std::string_view k_predecessor_name =
    "managed_child_unpublish_order_predecessor";
constexpr std::string_view k_replacement_process_name =
    "managed_child_unpublish_order_replacement";
constexpr auto k_step_timeout            = std::chrono::seconds(8);
constexpr auto k_lock_held_release_delay = std::chrono::seconds(5);
constexpr auto k_watchdog_timeout        = std::chrono::seconds(45);

constexpr sintra::instance_id_type k_reused_process_iid =
    sintra::compose_instance(42u, 1ull);
constexpr sintra::instance_id_type k_observer_process_iid =
    sintra::compose_instance(43u, 1ull);

class Watchdog
{
public:
    explicit Watchdog(const char* label)
    :
        m_label(label),
        m_deadline(std::chrono::steady_clock::now() + k_watchdog_timeout),
        m_thread([this]() {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_changed.wait_until(lock, m_deadline, [this]() { return m_done; })) {
                std::fprintf(
                    stderr,
                    "managed_child_stale_unpublish: watchdog expired in %s\n",
                    m_label);
                std::fflush(stderr);
                std::_Exit(124);
            }
        })
    {}

    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done = true;
        }
        m_changed.notify_all();
        m_thread.join();
    }

    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    const char*                           m_label;
    std::chrono::steady_clock::time_point m_deadline;
    std::mutex                            m_mutex;
    std::condition_variable               m_changed;
    bool                                  m_done = false;
    std::thread                           m_thread;
};

class Observation_service : public sintra::Derived_transceiver<Observation_service>
{
public:
    void record(std::string_view event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_order.empty()) {
            m_order.push_back(',');
        }
        m_order.append(event);
    }

    std::string snapshot(bool resolve_before_inspection)
    {
        if (resolve_before_inspection) {
            (void)sintra::get_instance_id<Observation_service>(
                std::string(k_replacement_process_name));
        }

        sintra::instance_id_type resolved = sintra::invalid_instance_id;
        {
            auto names = sintra::s_mproc->m_instance_id_of_assigned_name.scoped();
            const auto found = names.get().find(
                std::string(k_replacement_process_name));
            if (found != names.get().end()) {
                resolved = found->second;
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        return
            m_order + "|" +
            std::to_string(static_cast<unsigned long long>(resolved));
    }

    bool finish()
    {
        if (!sintra::s_mproc) {
            return false;
        }
        sintra::s_mproc->run_after_current_handler([this]() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_finished = true;
            }
            m_changed.notify_all();
        });
        return true;
    }

    bool wait_until_finished(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_until(lock, deadline, [this]() { return m_finished; });
    }

    SINTRA_RPC(snapshot)
    SINTRA_RPC(finish)

private:
    std::mutex                 m_mutex;
    std::condition_variable    m_changed;
    std::string                m_order;
    bool                       m_finished = false;
};

class Observation_ack : public sintra::Derived_transceiver<Observation_ack>
{
public:
    void acknowledge_b(bool cache_exact)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_b_seen = true;
            m_cache_exact = cache_exact;
        }
        m_changed.notify_all();
    }

    bool wait_for_b(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return
            m_changed.wait_until(lock, deadline, [this]() { return m_b_seen; }) &&
            m_cache_exact;
    }

    SINTRA_UNICAST(acknowledge_b)

private:
    std::mutex                 m_mutex;
    std::condition_variable    m_changed;
    bool                       m_b_seen      = false;
    bool                       m_cache_exact = false;
};

struct Retirement_gate
{
    std::mutex                 mutex;
    std::condition_variable    changed;
    bool                       entered       = false;
    bool                       release       = false;
    bool                       auto_released = false;
};

struct Threaded_result
{
    std::mutex                 mutex;
    std::condition_variable    changed;
    bool                       done  = false;
    bool                       value = false;
    bool                       threw = false;
};

struct Observer_snapshot
{
    bool                       valid    = false;
    std::string                order;
    sintra::instance_id_type   resolved = sintra::invalid_instance_id;
};

struct Notification_order_result
{
    bool                       valid                      = false;
    bool                       a_unpublished              = false;
    bool                       b_published                = false;
    bool                       b_completed_while_a_parked = false;
    bool                       b_observer_acknowledged    = false;
    bool                       a_hook_auto_released       = false;
    bool                       first_snapshot_valid       = false;
    bool                       final_snapshot_valid       = false;
    bool                       final_order_exact          = false;
    bool                       final_name_exact           = false;
    bool                       b_cleanup                  = false;
    bool                       observer_released          = false;
    bool                       all_custodies_released     = false;
    bool                       no_survivors               = false;
    std::string                final_order;
    sintra::instance_id_type   final_resolved             = sintra::invalid_instance_id;
};

std::atomic<unsigned> s_retirement_count{0};
sintra::instance_id_type s_expected_iid = sintra::invalid_instance_id;
std::string s_expected_name;
Retirement_gate* s_retirement_gate = nullptr;

void observe_name_retirement(
    sintra::instance_id_type   instance_id,
    const std::string&         assigned_name)
{
    if (instance_id == s_expected_iid && assigned_name == s_expected_name) {
        s_retirement_count.fetch_add(1, std::memory_order_release);
    }
}

void hold_predecessor_retirement(
    sintra::instance_id_type   instance_id,
    const std::string&         assigned_name)
{
    if (instance_id   != k_reused_process_iid ||
        assigned_name != k_predecessor_name)
    {
        return;
    }

    Retirement_gate* gate = s_retirement_gate;
    if (!gate) {
        return;
    }

    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->entered = true;
    gate->changed.notify_all();
    const auto deadline =
        std::chrono::steady_clock::now() + k_lock_held_release_delay;
    if (!gate->changed.wait_until(
            lock, deadline, [&]() { return gate->release; }))
    {
        gate->auto_released = true;
        gate->changed.notify_all();
    }
}

bool wait_for_retirement_entry(Retirement_gate& gate)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.changed.wait_until(
        lock,
        std::chrono::steady_clock::now() + k_step_timeout,
        [&]() { return gate.entered; });
}

void release_retirement(Retirement_gate& gate)
{
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.changed.notify_all();
}

bool wait_for_threaded_result(
    Threaded_result&                       result,
    std::chrono::steady_clock::time_point  deadline)
{
    std::unique_lock<std::mutex> lock(result.mutex);
    return result.changed.wait_until(
        lock, deadline, [&]() { return result.done; });
}

class Scoped_current_message
{
public:
    explicit Scoped_current_message(sintra::Message_prefix* message) noexcept
        : m_previous(sintra::s_tl_current_message)
    {
        sintra::s_tl_current_message = message;
    }

    ~Scoped_current_message() noexcept
    {
        sintra::s_tl_current_message = m_previous;
    }

    Scoped_current_message(const Scoped_current_message&)            = delete;
    Scoped_current_message& operator=(const Scoped_current_message&) = delete;

private:
    sintra::Message_prefix* m_previous;
};

sintra::instance_id_type resolve_exact(
    const std::string&         assigned_name,
    sintra::instance_id_type   process_iid,
    std::uint32_t              occurrence)
{
    std::atomic<bool> cancelled{false};
    return
        sintra::detail::Managed_child_readiness_access::resolve(
            sintra::s_coord,
            assigned_name,
            k_custody_identity,
            process_iid,
            occurrence,
            cancelled);
}

struct Publication_snapshot
{
    bool registry_present = false;
};

Publication_snapshot publication_snapshot(
    sintra::instance_id_type   process_iid,
    sintra::instance_id_type   instance_iid)
{
    Publication_snapshot snapshot;
    std::lock_guard lock(sintra::s_coord->m_publish_mutex);

    const auto process = sintra::s_coord->m_transceiver_registry.find(process_iid);
    snapshot.registry_present =
        process != sintra::s_coord->m_transceiver_registry.end() &&
        process->second.count(instance_iid) != 0;

    return snapshot;
}

sintra::instance_id_type resolve_ordinary(const std::string& assigned_name)
{
    return sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id, assigned_name);
}

bool no_managed_child_authority()
{
    if (!sintra::s_mproc) {
        return false;
    }

    bool custody_empty = false;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
        custody_empty = sintra::s_mproc->m_child_custodies.empty() &&
            sintra::s_mproc->m_child_custody_by_process.empty();
    }

    bool cached_spawns_empty = false;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_cached_spawns_mutex);
        cached_spawns_empty = sintra::s_mproc->m_cached_spawns.empty();
    }

    bool lifelines_empty = false;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_lifeline_mutex);
        lifelines_empty = sintra::s_mproc->m_lifeline_writes.empty();
    }

#ifndef _WIN32
    bool reap_roster_empty = false;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_spawned_child_pids_mutex);
        reap_roster_empty = sintra::s_mproc->m_spawned_child_pids.empty();
    }
#else
    const bool reap_roster_empty = true;
#endif

    return
        custody_empty       &&
        cached_spawns_empty &&
        lifelines_empty     &&
        reap_roster_empty;
}

Observer_snapshot parse_observer_snapshot(const std::string& encoded)
{
    Observer_snapshot snapshot;
    const auto separator = encoded.rfind('|');
    if (separator == std::string::npos) {
        return snapshot;
    }

    try {
        snapshot.order    = encoded.substr(0, separator);
        snapshot.resolved = static_cast<sintra::instance_id_type>(
            std::stoull(encoded.substr(separator + 1)));
        snapshot.valid    = true;
    }
    catch (...) {
    }
    return snapshot;
}

Observer_snapshot observe_remote_snapshot(bool resolve_before_inspection)
{
    try {
        auto request = Observation_service::rpc_async_snapshot(
            std::string(k_observer_name), resolve_before_inspection);
        return parse_observer_snapshot(request.get_until(
            std::chrono::steady_clock::now() + k_step_timeout));
    }
    catch (...) {
        return {};
    }
}

bool finish_observer()
{
    try {
        auto request = Observation_service::rpc_async_finish(
            std::string(k_observer_name));
        return request.get_until(
            std::chrono::steady_clock::now() + k_step_timeout);
    }
    catch (...) {
        return false;
    }
}

std::shared_ptr<sintra::detail::Managed_child_custody_record>
make_synthetic_custody()
{
    auto custody  = sintra::s_mproc->accept_child_custody();
    bool admitted = false;
    {
        auto launch = sintra::s_mproc->admit_child_custody_occurrence(
            custody, k_reused_process_iid, 0);
        admitted = static_cast<bool>(launch);
    }
    if (!admitted) {
        return {};
    }

    std::lock_guard<std::mutex> lock(custody->mutex);
    const auto* occurrence = custody->find_occurrence_locked(
        k_reused_process_iid, 0);
    if (!occurrence ||
        occurrence->setup !=
            sintra::detail::Managed_child_occurrence_record::setup_state::no_child)
    {
        return {};
    }
    return custody;
}

bool publish_synthetic_process(
    const std::shared_ptr<sintra::detail::Managed_child_custody_record>&
                           custody,
    std::string_view       assigned_name)
{
    return
        custody &&
        sintra::s_coord->publish_managed_child_transceiver_for_test(
            sintra::make_user_type_id(1004),
            k_reused_process_iid,
            std::string(assigned_name),
            custody->identity,
            k_reused_process_iid,
            0) == k_reused_process_iid;
}

bool exact_unpublish_synthetic_process(
    const std::shared_ptr<sintra::detail::Managed_child_custody_record>& custody)
{
    if (!custody) {
        return false;
    }

    sintra::Message_prefix request{};
    request.message_type_id                = static_cast<sintra::type_id_type>(
        sintra::detail::reserved_id::unpublish_transceiver);
    request.sender_instance_id             = k_reused_process_iid;
    request.receiver_instance_id           = sintra::s_coord_id;
    request.managed_child_custody_identity = custody->identity;
    request.managed_child_occurrence       = 0;

    Scoped_current_message current_message(&request);
    return sintra::Coordinator::rpc_unpublish_transceiver(
        sintra::s_coord_id, k_reused_process_iid);
}

bool wait_for_synthetic_release(
    const std::shared_ptr<sintra::detail::Managed_child_custody_record>& custody)
{
    if (!custody) {
        return false;
    }
    std::unique_lock<std::mutex> lock(custody->mutex);
    return custody->changed.wait_until(
        lock,
        std::chrono::steady_clock::now() + k_step_timeout,
        [&]() { return custody->release_state.released(); });
}

int run_observer(int argc, char* argv[])
{
    Watchdog watchdog("observer");
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    bool assigned = false;
    bool finished = false;
    {
        Observation_service observer;
        const auto ack_iid = sintra::Coordinator::rpc_resolve_instance(
            sintra::s_coord_id, std::string(k_observer_ack_name));
        auto published = [&observer, ack_iid](
            const sintra::Coordinator::instance_published& event)
        {
            if (event.instance_id == k_reused_process_iid &&
                static_cast<std::string>(event.assigned_name) ==
                    k_replacement_process_name)
            {
                const auto resolved =
                    sintra::get_instance_id<Observation_service>(
                        std::string(k_replacement_process_name));
                observer.record("B+");
                Observation_ack::rpc_acknowledge_b(
                    ack_iid, resolved == k_reused_process_iid);
            }
        };
        auto unpublished = [&observer](
            const sintra::Coordinator::instance_unpublished& event)
        {
            if (event.instance_id == k_reused_process_iid &&
                static_cast<std::string>(event.assigned_name) ==
                    k_predecessor_name)
            {
                observer.record("A-");
            }
        };
        sintra::activate_slot(
            published,
            sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));
        sintra::activate_slot(
            unpublished,
            sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

        assigned = ack_iid != sintra::invalid_instance_id &&
            observer.assign_name(std::string(k_observer_name));
        if (assigned) {
            sintra::Coordinator::rpc_mark_initialization_complete(
                sintra::s_coord_id, sintra::s_mproc_id);
        }
        finished = assigned && observer.wait_until_finished(
            std::chrono::steady_clock::now() +
                k_watchdog_timeout - std::chrono::seconds(5));
        sintra::deactivate_all_slots();
    }

    bool finalized = false;
    try {
        finalized = sintra::detail::finalize();
    }
    catch (...) {
    }
    return assigned && finished && finalized ? 0 : 3;
}

Notification_order_result run_notification_order_contract(
    const std::string& binary_path)
{
    Notification_order_result result;
    Observation_ack observer_ack;
    const bool ack_available = observer_ack.assign_name(
        std::string(k_observer_ack_name));

    sintra::Spawn_options observer_options;
    observer_options.binary_path              = binary_path;
    observer_options.args                     = {std::string(k_observer_flag)};
    observer_options.process_instance_id      = k_observer_process_iid;
    observer_options.readiness_instance_name  = std::string(k_observer_name);
    observer_options.lifetime.enable_lifeline = false;

    auto observer = sintra::spawn_swarm_process(observer_options);
    const auto observer_ready = observer.wait_for_readiness_until(
        std::chrono::steady_clock::now() + k_step_timeout);
    const bool observer_available = ack_available && observer &&
        observer_ready.readiness_state ==
            sintra::Managed_child_readiness_state::reached;

    auto custody_a = observer_available ? make_synthetic_custody() : nullptr;
    const bool a_published = publish_synthetic_process(
        custody_a, k_predecessor_name);
    result.a_unpublished = false;

    std::shared_ptr<sintra::detail::Managed_child_custody_record> custody_b;
    Retirement_gate retirement;
    Threaded_result a_call;
    Threaded_result b_call;
    Observer_snapshot b_snapshot;
    std::thread a_thread;
    std::thread b_thread;

    if (a_published) {
        s_retirement_gate = &retirement;
        {
            sintra::test::managed_child::Scoped_test_hook retirement_hook(
                sintra::detail::test_hooks::s_coordinator_name_retired,
                &hold_predecessor_retirement);

            a_thread = std::thread([&]() {
                try {
                    a_call.value = exact_unpublish_synthetic_process(custody_a);
                }
                catch (...) {
                    a_call.threw = true;
                }
                {
                    std::lock_guard<std::mutex> lock(a_call.mutex);
                    a_call.done = true;
                }
                a_call.changed.notify_all();
            });

            const bool a_parked = wait_for_retirement_entry(retirement);
            if (a_parked) {
                custody_b = make_synthetic_custody();
                b_thread = std::thread([&]() {
                    try {
                        b_call.value = publish_synthetic_process(
                            custody_b, k_replacement_process_name);
                    }
                    catch (...) {
                        b_call.threw = true;
                    }
                    {
                        std::lock_guard<std::mutex> lock(b_call.mutex);
                        b_call.done = true;
                    }
                    b_call.changed.notify_all();
                });

                const bool b_done = wait_for_threaded_result(
                    b_call,
                    std::chrono::steady_clock::now() + k_step_timeout);
                bool auto_released = false;
                {
                    std::lock_guard<std::mutex> lock(retirement.mutex);
                    auto_released = retirement.auto_released;
                }
                result.b_completed_while_a_parked = b_done && !auto_released;
                result.a_hook_auto_released       = auto_released;
                result.b_published                = b_done && b_call.value && !b_call.threw;
                if (result.b_published) {
                    result.b_observer_acknowledged = observer_ack.wait_for_b(
                        std::chrono::steady_clock::now() + k_step_timeout);
                    if (result.b_observer_acknowledged) {
                        b_snapshot = observe_remote_snapshot(false);
                    }
                    result.first_snapshot_valid =
                        result.b_observer_acknowledged && b_snapshot.valid &&
                        b_snapshot.resolved == k_reused_process_iid &&
                        b_snapshot.order.find("B+") != std::string::npos;
                }
            }

            release_retirement(retirement);
            if (a_thread.joinable()) { a_thread.join(); }
            if (b_thread.joinable()) { b_thread.join(); }
        }
        s_retirement_gate = nullptr;
    }

    result.a_unpublished = a_call.done && a_call.value && !a_call.threw;
    const auto final = result.b_published
        ? observe_remote_snapshot(false)
        : Observer_snapshot{};
    result.final_snapshot_valid = final.valid;
    result.final_order          = final.order;
    result.final_resolved       = final.resolved;
    result.final_order_exact    = final.valid && final.order == "A-,B+";
    result.final_name_exact     = final.valid &&
        final.resolved == k_reused_process_iid;

    result.b_cleanup = custody_b && exact_unpublish_synthetic_process(custody_b);
    if (custody_a) {
        sintra::s_mproc->request_child_custody_release(custody_a);
    }
    if (custody_b) {
        sintra::s_mproc->request_child_custody_release(custody_b);
    }
    const bool a_released = wait_for_synthetic_release(custody_a);
    const bool b_released = wait_for_synthetic_release(custody_b);

    const bool observer_finish_requested = observer_available && finish_observer();
    auto observer_release = observer.release_until(
        std::chrono::steady_clock::now() + k_step_timeout);
    if (observer_release.release_state !=
        sintra::Managed_child_release_state::complete)
    {
        observer_release = observer.terminate_until(
            std::chrono::steady_clock::now() + k_step_timeout);
    }
    result.observer_released      = observer_finish_requested &&
        observer_release.release_state ==
            sintra::Managed_child_release_state::complete;
    result.all_custodies_released = sintra::s_mproc->wait_for_all_child_custodies(
        std::chrono::steady_clock::now() + k_step_timeout);
    result.no_survivors           = no_managed_child_authority();

    result.valid = observer_available && a_published && result.a_unpublished &&
        result.b_published && result.b_observer_acknowledged &&
        result.first_snapshot_valid &&
        result.final_snapshot_valid && result.final_order_exact &&
        result.final_name_exact && result.b_cleanup && a_released && b_released &&
        result.observer_released && result.all_custodies_released &&
        result.no_survivors;
    return result;
}

} // namespace

int main(int argc, char* argv[])
{
    if (sintra::test::has_argv_flag(argc, argv, k_observer_flag)) {
        return run_observer(argc, argv);
    }

    Watchdog watchdog("coordinator");
    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    if (binary_path.empty()) {
        return 2;
    }
    sintra::init(argc, argv);

    const auto process_iid = sintra::s_mproc_id;
    const auto replacement_iid = sintra::compose_instance(
        sintra::get_process_index(process_iid),
        k_transceiver_index);
    const std::string replacement_name =
        "managed_child_stale_unpublish_replacement_" +
        std::to_string(static_cast<unsigned long long>(process_iid));

    s_expected_iid = replacement_iid;
    s_expected_name = replacement_name;
    s_retirement_count.store(0, std::memory_order_relaxed);

    const auto publish_result =
        sintra::s_coord->publish_managed_child_transceiver_for_test(
            sintra::make_user_type_id(1003),
            replacement_iid,
            replacement_name,
            k_custody_identity,
            process_iid,
            k_replacement_occurrence);
    const auto before = publication_snapshot(process_iid, replacement_iid);
    const bool baseline_valid = publish_result == replacement_iid &&
        resolve_ordinary(replacement_name) == replacement_iid &&
        resolve_exact(
            replacement_name, process_iid, k_replacement_occurrence) ==
                replacement_iid &&
        resolve_exact(replacement_name, process_iid, k_stale_occurrence) ==
            sintra::invalid_instance_id &&
        before.registry_present && no_managed_child_authority();

    bool stale_result = false;
    sintra::Message_prefix* const previous_message = sintra::s_tl_current_message;
    {
        sintra::test::managed_child::Scoped_test_hook retirement_hook(
            sintra::detail::test_hooks::s_coordinator_name_retired,
            &observe_name_retirement);

        sintra::Message_prefix stale_request{};
        stale_request.message_type_id                = static_cast<sintra::type_id_type>(
            sintra::detail::reserved_id::unpublish_transceiver);
        stale_request.sender_instance_id             = process_iid;
        stale_request.receiver_instance_id           = sintra::s_coord_id;
        stale_request.managed_child_custody_identity = k_custody_identity;
        stale_request.managed_child_occurrence       = k_stale_occurrence;

        Scoped_current_message current_message(&stale_request);
        stale_result = sintra::Coordinator::rpc_unpublish_transceiver(
            sintra::s_coord_id, replacement_iid);
    }

    const bool tls_restored = sintra::s_tl_current_message == previous_message;
    const bool hook_restored =
        sintra::detail::test_hooks::s_coordinator_name_retired.load(
            std::memory_order_acquire) == nullptr;
    const unsigned retirement_count =
        s_retirement_count.load(std::memory_order_acquire);
    const auto after_stale          = publication_snapshot(process_iid, replacement_iid);
    const auto ordinary_after_stale = resolve_ordinary(replacement_name);
    const auto replacement_exact_after_stale = resolve_exact(
        replacement_name, process_iid, k_replacement_occurrence);
    const auto stale_exact_after_stale = resolve_exact(
        replacement_name, process_iid, k_stale_occurrence);

    const bool fixed_green = baseline_valid && !stale_result &&
        retirement_count == 0 && ordinary_after_stale == replacement_iid &&
        replacement_exact_after_stale == replacement_iid &&
        stale_exact_after_stale == sintra::invalid_instance_id &&
        after_stale.registry_present && tls_restored && hook_restored;

    const bool current_red = baseline_valid && stale_result &&
        retirement_count == 1 &&
        ordinary_after_stale == sintra::invalid_instance_id &&
        replacement_exact_after_stale == sintra::invalid_instance_id &&
        stale_exact_after_stale == sintra::invalid_instance_id &&
        after_stale.registry_present && tls_restored && hook_restored;

    // GREEN still owns the replacement publication; RED has already damaged
    // it. Successful cleanup must retire the complete owning registry record.
    const bool cleanup_result = sintra::Coordinator::rpc_unpublish_transceiver(
        sintra::s_coord_id, replacement_iid);
    const auto after_cleanup = publication_snapshot(process_iid, replacement_iid);
    const bool cleanup_valid = cleanup_result && !after_cleanup.registry_present &&
        resolve_ordinary(replacement_name) == sintra::invalid_instance_id &&
        resolve_exact(
            replacement_name, process_iid, k_replacement_occurrence) ==
                sintra::invalid_instance_id &&
        !after_cleanup.registry_present && no_managed_child_authority();

    const auto notification_order = cleanup_valid
        ? run_notification_order_contract(binary_path)
        : Notification_order_result{};
    const bool notification_order_red =
        notification_order.a_unpublished                                 &&
        notification_order.b_published                                   &&
        notification_order.b_completed_while_a_parked                    &&
        notification_order.b_observer_acknowledged                       &&
        notification_order.first_snapshot_valid                          &&
        notification_order.final_snapshot_valid                          &&
        notification_order.final_order == "B+,A-"                        &&
        notification_order.final_resolved == sintra::invalid_instance_id &&
        notification_order.b_cleanup                                     &&
        notification_order.observer_released                             &&
        notification_order.all_custodies_released                        &&
        notification_order.no_survivors;

    const bool finalized    = sintra::detail::finalize();
    const bool runtime_gone = sintra::s_mproc == nullptr && sintra::s_coord == nullptr;

    if (current_red && cleanup_valid && notification_order.valid &&
        finalized   && runtime_gone)
    {
        std::fprintf(stderr,
            "STALE_UNPUBLISH_RED custody=%llu piid=%llu iid=%llu "
            "occurrence_n=%u occurrence_n1=%u stale_result=1 retired=1 "
            "ordinary_n1=0 exact_n1=0 registry_cell_n1=1 identity_n1=0 "
            "cleanup=1 finalized=1 native_children=0 hooks_restored=1 "
            "tls_restored=1\n",
            static_cast<unsigned long long>(k_custody_identity),
            static_cast<unsigned long long>(process_iid),
            static_cast<unsigned long long>(replacement_iid),
            k_stale_occurrence,
            k_replacement_occurrence);
        std::fflush(stderr);
        return 87;
    }

    if (fixed_green && cleanup_valid && notification_order_red &&
        finalized   && runtime_gone)
    {
        std::fprintf(
            stderr,
            "UNPUBLISH_NOTIFICATION_ORDER_RED order=%s resolved=%llu "
            "b_while_a_parked=1 observer_ack=1 cleanup=1 "
            "observer_released=1 survivors=0\n",
            notification_order.final_order.c_str(),
            static_cast<unsigned long long>(notification_order.final_resolved));
        std::fflush(stderr);
        return 88;
    }

    if (fixed_green && cleanup_valid && notification_order.valid &&
        finalized   && runtime_gone)
    {
        std::fprintf(stdout,
            "STALE_UNPUBLISH_GREEN custody=%llu piid=%llu iid=%llu "
            "occurrence_n=%u occurrence_n1=%u stale_result=0 retired=0 "
            "ordinary_n1=1 exact_n1=1 registry_cell_n1=1 identity_n1=1 "
            "cleanup=1 finalized=1 native_children=0 hooks_restored=1 "
            "tls_restored=1 notification_order=A-,B+ peer_name=1 "
            "observer_released=1 survivors=0\n",
            static_cast<unsigned long long>(k_custody_identity),
            static_cast<unsigned long long>(process_iid),
            static_cast<unsigned long long>(replacement_iid),
            k_stale_occurrence,
            k_replacement_occurrence);
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(stderr,
        "STALE_UNPUBLISH_INVALID baseline=%d stale_result=%d retired=%u "
        "ordinary_n1=%d exact_n1=%d stale_exact=%d registry_n1=%d "
        "identity_n1=%d identity_exact=%d tls_restored=%d hook_restored=%d "
        "fixed_green=%d current_red=%d cleanup_result=%d registry_absent=%d "
        "cleanup_valid=%d "
        "notification_valid=%d notification_red=%d order=%s resolved=%llu "
        "b_while_a_parked=%d hook_auto_release=%d observer_ack=%d "
        "first_snapshot=%d final_snapshot=%d b_cleanup=%d observer_released=%d "
        "all_custodies=%d survivors_absent=%d finalized=%d runtime_gone=%d\n",
        baseline_valid ? 1 : 0,
        stale_result ? 1 : 0,
        retirement_count,
        ordinary_after_stale == replacement_iid ? 1 : 0,
        replacement_exact_after_stale == replacement_iid ? 1 : 0,
        stale_exact_after_stale == sintra::invalid_instance_id ? 1 : 0,
        after_stale.registry_present ? 1 : 0,
        replacement_exact_after_stale != sintra::invalid_instance_id ? 1 : 0,
        replacement_exact_after_stale == replacement_iid ? 1 : 0,
        tls_restored ? 1 : 0,
        hook_restored ? 1 : 0,
        fixed_green ? 1 : 0,
        current_red ? 1 : 0,
        cleanup_result ? 1 : 0,
        !after_cleanup.registry_present ? 1 : 0,
        cleanup_valid ? 1 : 0,
        notification_order.valid ? 1 : 0,
        notification_order_red ? 1 : 0,
        notification_order.final_order.c_str(),
        static_cast<unsigned long long>(notification_order.final_resolved),
        notification_order.b_completed_while_a_parked ? 1 : 0,
        notification_order.a_hook_auto_released ? 1 : 0,
        notification_order.b_observer_acknowledged ? 1 : 0,
        notification_order.first_snapshot_valid ? 1 : 0,
        notification_order.final_snapshot_valid ? 1 : 0,
        notification_order.b_cleanup ? 1 : 0,
        notification_order.observer_released ? 1 : 0,
        notification_order.all_custodies_released ? 1 : 0,
        notification_order.no_survivors ? 1 : 0,
        finalized ? 1 : 0,
        runtime_gone ? 1 : 0);
    std::fflush(stderr);
    return 3;
}
