//
// Sintra library, example 6
//
// This example demonstrates how to dynamically add and remove processes from a
// running swarm. Two workers begin a ping-pong exchange. After a configurable
// number of rallies, a third process is spawned and participates in a three-way
// rotation. The coordinator then asks the third participant to leave, waits for
// a few more rallies, and brings it back before shutting everything down.
//

#include <sintra/sintra.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>

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

std::string g_binary_path;

Role detect_role(int argc, char* const argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const std::string option = "--role";
        if (arg.rfind(option + "=", 0) == 0) {
            arg = arg.substr(option.size() + 1);
        }
        else if (arg == option && i + 1 < argc) {
            arg = argv[++i];
        }
        else {
            continue;
        }

        if (arg == "ping") {
            return Role::Ping;
        }
        if (arg == "pong") {
            return Role::Pong;
        }
        if (arg == "extra") {
            return Role::Extra;
        }
    }

    return Role::Coordinator;
}

bool spawn_role(const std::string& role_name)
{
    return spawn_swarm_process(g_binary_path, {"--role", role_name}) == 1;
}

int run_worker(int role_index)
{
    std::atomic<int> participant_count{role_index == kExtraRole ? 3 : 2};
    std::atomic<bool> exit_requested{false};
    std::atomic<bool> stop_requested{false};

    std::mutex wait_mutex;
    std::condition_variable wait_cv;

    activate_slot([&](Participant_count update) {
        participant_count.store(update.participants, std::memory_order_release);
    });

    activate_slot([&](Ball ball) {
        if (ball.target_role != role_index) {
            return;
        }
        if (exit_requested.load(std::memory_order_acquire) ||
            stop_requested.load(std::memory_order_acquire)) {
            return;
        }

        const auto participants = participant_count.load(std::memory_order_acquire);
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
        if (!exit_requested.exchange(true, std::memory_order_acq_rel)) {
            world() << Role_exited{role_index};
            std::lock_guard<std::mutex> lk(wait_mutex);
            wait_cv.notify_one();
        }
    });

    activate_slot([&](Stop) {
        stop_requested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(wait_mutex);
        wait_cv.notify_one();
    });

    world() << Role_ready{role_index};

    std::unique_lock<std::mutex> lk(wait_mutex);
    wait_cv.wait(lk, [&] {
        return exit_requested.load(std::memory_order_acquire) ||
               stop_requested.load(std::memory_order_acquire);
    });
    lk.unlock();

    if (stop_requested.load(std::memory_order_acquire)) {
        barrier("dynamic-swarm-finished", "_sintra_all_processes");
    }

    finalize();
    return 0;
}

void run_coordinator()
{
    std::mutex state_mutex;
    std::condition_variable state_cv;

    std::set<int> ready_roles;
    std::uint64_t total_exchanges = 0;
    bool extra_exited = false;
    bool barrier_ready = false;

    auto stop_and_finalize = [&](bool send_stop) {
        if (send_stop) {
            world() << Stop{};
        }
        if (barrier_ready) {
            barrier("dynamic-swarm-finished", "_sintra_all_processes");
        }
        finalize();
    };

    activate_slot([&](Role_ready ready) {
        std::lock_guard<std::mutex> lk(state_mutex);
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

    console() << "Launching ping and pong workers\n";
    if (!spawn_role("ping") || !spawn_role("pong")) {
        console() << "Failed to spawn initial workers\n";
        finalize();
        return;
    }

    wait_for_roles({kPingRole, kPongRole});
    barrier_ready = true;

    console() << "Starting ping-pong with two participants\n";
    world() << Participant_count{2};
    world() << Ball{0, kPingRole};

    constexpr std::uint64_t kSpawnExtraAfter = 12;
    constexpr std::uint64_t kThreeWayRallies = 9;
    constexpr std::uint64_t kTwoWayRalliesBetweenRejoins = 8;

    wait_for_exchanges(kSpawnExtraAfter);

    console() << "Spawning third participant\n";
    if (!spawn_role("extra")) {
        console() << "Failed to spawn extra participant\n";
        stop_and_finalize(true);
        return;
    }

    wait_for_roles({kExtraRole});
    std::uint64_t exchanges_at_three_start;
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        exchanges_at_three_start = total_exchanges;
    }

    world() << Participant_count{3};
    wait_for_exchanges(exchanges_at_three_start + kThreeWayRallies);

    console() << "Requesting extra participant to leave\n";
    world() << Role_exit_request{kExtraRole};
    wait_for_extra_exit();

    world() << Participant_count{2};
    std::uint64_t exchanges_after_exit;
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        exchanges_after_exit = total_exchanges;
    }

    wait_for_exchanges(exchanges_after_exit + kTwoWayRalliesBetweenRejoins);

    console() << "Re-spawning extra participant\n";
    if (!spawn_role("extra")) {
        console() << "Failed to respawn extra participant\n";
        stop_and_finalize(true);
        return;
    }

    wait_for_roles({kExtraRole});
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        exchanges_at_three_start = total_exchanges;
    }

    world() << Participant_count{3};
    wait_for_exchanges(exchanges_at_three_start + kThreeWayRallies);

    console() << "Final request for extra participant to leave\n";
    world() << Role_exit_request{kExtraRole};
    wait_for_extra_exit();
    world() << Participant_count{2};

    std::uint64_t final_target;
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        final_target = total_exchanges + 6;
    }
    wait_for_exchanges(final_target);

    console() << "Stopping demonstration\n";
    stop_and_finalize(true);
}

} // namespace

int main(int argc, char* argv[])
{
    g_binary_path = argv[0];
    const Role role = detect_role(argc, argv);

    init(argc, argv);

    switch (role) {
    case Role::Coordinator:
        run_coordinator();
        break;
    case Role::Ping:
        return run_worker(kPingRole);
    case Role::Pong:
        return run_worker(kPongRole);
    case Role::Extra:
        return run_worker(kExtraRole);
    }

    return 0;
}

