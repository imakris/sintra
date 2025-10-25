//
// Sintra Dynamic Swarm Test
//
// This test mirrors example 6 (dynamic swarm) by coordinating a ping/pong
// exchange where an extra participant repeatedly joins and leaves. Unlike the
// standalone example that spawns new binaries, this test uses a fixed set of
// processes created via Process_descriptor and toggles participation to model
// dynamic membership. It validates that message routing, role readiness,
// exit notifications, and barrier coordination behave as expected.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace sintra;

namespace {

enum class Role
{
    Coordinator,
    Ping,
    Pong,
    Extra,
};

struct Ball
{
    std::uint64_t exchange;
    int target_role;
};

struct Participant_count
{
    int participants;
};

struct Exchange_report
{
    std::uint64_t exchange;
    int role;
};

struct Role_ready
{
    int role;
};

struct Role_exit_request
{
    int role;
};

struct Role_exited
{
    int role;
};

struct Stop
{};

constexpr int kPingRole = 0;
constexpr int kPongRole = 1;
constexpr int kExtraRole = 2;

int run_worker(int role_index)
{
    const bool is_extra = (role_index == kExtraRole);

    std::atomic<int> participant_count{2};
    std::atomic<bool> active{!is_extra};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> ready_sent{false};

    std::mutex wait_mutex;
    std::condition_variable wait_cv;

    auto announce_ready = [&]() {
        if (!active.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (ready_sent.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            world() << Role_ready{role_index};
        }
    };

    if (!is_extra) {
        announce_ready();
    }

    activate_slot([&](Participant_count update) {
        participant_count.store(update.participants, std::memory_order_release);
        if (role_index < update.participants) {
            active.store(true, std::memory_order_release);
            if (is_extra) {
                ready_sent.store(false, std::memory_order_release);
                announce_ready();
            }
        }
        else {
            active.store(false, std::memory_order_release);
            if (is_extra) {
                ready_sent.store(false, std::memory_order_release);
            }
        }
    });

    activate_slot([&](Ball ball) {
        if (ball.target_role != role_index) {
            return;
        }
        if (!active.load(std::memory_order_acquire) ||
            stop_requested.load(std::memory_order_acquire)) {
            return;
        }

        const int participants = participant_count.load(std::memory_order_acquire);
        if (participants <= 0 || role_index >= participants) {
            return;
        }

        const std::uint64_t next_exchange = ball.exchange + 1;
        const int next_role = (role_index + 1) % participants;

        world() << Exchange_report{next_exchange, role_index};
        world() << Ball{next_exchange, next_role};
    });

    activate_slot([&](Role_exit_request exit) {
        if (exit.role != role_index) {
            return;
        }
        active.store(false, std::memory_order_release);
        if (is_extra) {
            ready_sent.store(false, std::memory_order_release);
        }
        world() << Role_exited{role_index};
    });

    activate_slot([&](Stop) {
        stop_requested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(wait_mutex);
        wait_cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(wait_mutex);
    wait_cv.wait(lk, [&] { return stop_requested.load(std::memory_order_acquire); });
    lk.unlock();

    barrier("dynamic-swarm-finished", "_sintra_all_processes");
    finalize();
    return 0;
}

int run_coordinator()
{
    std::mutex state_mutex;
    std::condition_variable state_cv;

    std::set<int> ready_roles;
    std::uint64_t total_exchanges = 0;
    bool extra_exited = false;
    bool barrier_ready = false;
    int extra_ready_events = 0;
    int extra_exit_events = 0;
    std::uint64_t expected_final_target = 0;

    auto stop_and_finalize = [&](bool send_stop) {
        if (send_stop) {
            world() << Stop{};
        }
        if (barrier_ready) {
            barrier("dynamic-swarm-finished", "_sintra_all_processes");
        }
        finalize();
    };

    auto finalize_and_return = [&](bool send_stop, int code) {
        stop_and_finalize(send_stop);
        return code;
    };

    activate_slot([&](Role_ready ready) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (ready.role == kExtraRole) {
            ++extra_ready_events;
        }
        ready_roles.insert(ready.role);
        state_cv.notify_all();
    });

    activate_slot([&](Exchange_report report) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (report.exchange > total_exchanges) {
            total_exchanges = report.exchange;
            state_cv.notify_all();
        }
    });

    activate_slot([&](Role_exited exited) {
        if (exited.role != kExtraRole) {
            return;
        }
        std::lock_guard<std::mutex> lk(state_mutex);
        extra_exited = true;
        ++extra_exit_events;
        ready_roles.erase(kExtraRole);
        state_cv.notify_all();
    });

    auto wait_for_roles = [&](std::initializer_list<int> roles) {
        std::unique_lock<std::mutex> lk(state_mutex);
        state_cv.wait(lk, [&] {
            for (int role : roles) {
                if (!ready_roles.count(role)) {
                    return false;
                }
            }
            return true;
        });
    };

    auto wait_for_exchanges = [&](std::uint64_t target) {
        std::unique_lock<std::mutex> lk(state_mutex);
        state_cv.wait(lk, [&] { return total_exchanges >= target; });
    };

    auto wait_for_extra_exit = [&] {
        std::unique_lock<std::mutex> lk(state_mutex);
        state_cv.wait(lk, [&] { return extra_exited; });
        extra_exited = false;
    };

    console() << "Waiting for ping and pong readiness\n";
    wait_for_roles({kPingRole, kPongRole});
    barrier_ready = true;

    console() << "Starting ping-pong with two participants\n";
    world() << Participant_count{2};
    world() << Ball{0, kPingRole};

    constexpr std::uint64_t kSpawnExtraAfter = 12;
    constexpr std::uint64_t kThreeWayRallies = 9;
    constexpr std::uint64_t kTwoWayRalliesBetweenRejoins = 8;

    wait_for_exchanges(kSpawnExtraAfter);

    console() << "Activating third participant\n";
    world() << Participant_count{3};
    wait_for_roles({kExtraRole});
    std::uint64_t exchanges_at_three_start = 0;
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        exchanges_at_three_start = total_exchanges;
    }

    wait_for_exchanges(exchanges_at_three_start + kThreeWayRallies);

    console() << "Requesting extra participant to pause\n";
    world() << Role_exit_request{kExtraRole};
    wait_for_extra_exit();

    world() << Participant_count{2};
    std::uint64_t exchanges_after_exit = 0;
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        exchanges_after_exit = total_exchanges;
    }

    wait_for_exchanges(exchanges_after_exit + kTwoWayRalliesBetweenRejoins);

    console() << "Re-activating extra participant\n";
    world() << Participant_count{3};
    wait_for_roles({kExtraRole});
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        exchanges_at_three_start = total_exchanges;
    }

    wait_for_exchanges(exchanges_at_three_start + kThreeWayRallies);

    console() << "Final request for extra participant to pause\n";
    world() << Role_exit_request{kExtraRole};
    wait_for_extra_exit();
    world() << Participant_count{2};

    {
        std::lock_guard<std::mutex> lk(state_mutex);
        expected_final_target = total_exchanges + 6;
    }
    wait_for_exchanges(expected_final_target);

    bool success = true;
    std::string failure_reason;
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (total_exchanges < expected_final_target) {
            success = false;
            failure_reason += "Total exchanges did not reach target.\n";
        }
        if (extra_ready_events != 2) {
            success = false;
            failure_reason += "Extra participant did not activate twice.\n";
        }
        if (extra_exit_events != 2) {
            success = false;
            failure_reason += "Extra participant did not pause twice.\n";
        }
    }

    if (!success) {
        console() << "Dynamic swarm validation failed: " << failure_reason;
    }
    else {
        console() << "Stopping demonstration\n";
    }

    return finalize_and_return(true, success ? 0 : 1);
}

int process_ping()
{
    return run_worker(kPingRole);
}

int process_pong()
{
    return run_worker(kPongRole);
}

int process_extra()
{
    return run_worker(kExtraRole);
}

int process_coordinator()
{
    return run_coordinator();
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_ping);
    processes.emplace_back(process_pong);
    processes.emplace_back(process_extra);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        process_coordinator();
        sintra::finalize();
    }

    return 0;
}

