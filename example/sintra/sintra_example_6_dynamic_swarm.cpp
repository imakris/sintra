//
// Sintra library, example 6
//
// Demonstrates dynamically inviting processes into an ongoing swarm.
// Two baseline players exchange "ball" messages. A bench player joins
// the rally mid-stream when the controller decides the warmup phase has
// completed. After a short three-way exchange, the bench player is asked
// to leave, and later re-enters for another cycle.
//

#include <sintra/sintra.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>

using namespace sintra;
using namespace std;

namespace {

constexpr int bench_branch_index = 3;

struct Ball {
    int target;
    std::uint64_t hit;
};

struct HitReport {
    int hitter;
    int roster_size;
    std::uint64_t sequence;
};

struct UpdateRoster {
    int count;
    int players[3];
};

struct PlayerReady {
    int index;
};

struct PlayerStopped {
    int index;
};

struct BenchReady {
    int index;
};

struct BenchDeparted {
    int index;
};

struct LeaveSwarm {
    int target;
};

struct StopPlay {};

UpdateRoster make_roster(std::initializer_list<int> players)
{
    UpdateRoster update{};
    update.count = static_cast<int>(players.size());
    int idx = 0;
    for (int value : players) {
        if (idx < 3) {
            update.players[idx++] = value;
        }
    }
    for (; idx < 3; ++idx) {
        update.players[idx] = 0;
    }
    return update;
}

int next_target(const std::array<int, 3>& roster, size_t roster_size, int me)
{
    if (roster_size == 0) {
        return me;
    }
    for (size_t idx = 0; idx < roster_size; ++idx) {
        if (roster[idx] == me) {
            const auto next_idx = (idx + 1) % roster_size;
            return roster[next_idx];
        }
    }
    return roster[0];
}

string roster_to_string(const std::array<int, 3>& roster, size_t roster_size)
{
    string result = "{";
    for (size_t i = 0; i < roster_size; ++i) {
        result += to_string(roster[i]);
        if (i + 1 < roster_size) {
            result += ", ";
        }
    }
    result += "}";
    return result;
}

int run_regular_player(const char* label)
{
    const int me = process_index();
    console() << "[" << label << "] joining the rally as process " << me << "\n";

    std::array<int, 3> roster{1, 2, 0};
    size_t roster_size = 2;
    std::atomic<bool> stop_requested{false};
    std::mutex stop_mutex;
    std::condition_variable stop_cv;

    activate_slot([&](UpdateRoster update) {
        roster_size = static_cast<size_t>(update.count);
        for (size_t idx = 0; idx < roster_size; ++idx) {
            roster[idx] = update.players[idx];
        }
        console() << "[" << label << "] roster updated to "
                  << roster_to_string(roster, roster_size) << "\n";
    });

    activate_slot([&](StopPlay) {
        stop_requested.store(true, std::memory_order_release);
        stop_cv.notify_one();
    });

    activate_slot([&](Ball ball) {
        if (ball.target != me) {
            return;
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            return;
        }

        const auto next = next_target(roster, roster_size, me);
        const auto next_hit = ball.hit + 1;

        console() << "[" << label << "] hit " << next_hit
                  << ", sending to player " << next << "\n";
        world() << HitReport{me, static_cast<int>(roster_size), next_hit};
        world() << Ball{next, next_hit};
    });

    world() << PlayerReady{me};

    std::unique_lock<std::mutex> lk(stop_mutex);
    stop_cv.wait(lk, [&] { return stop_requested.load(std::memory_order_acquire); });

    deactivate_all_slots();
    world() << PlayerStopped{me};
    console() << "[" << label << "] leaving the table\n";

    barrier("dynamic-pingpong-finished");
    return 0;
}

int player_one()
{
    return run_regular_player("Player 1");
}

int player_two()
{
    return run_regular_player("Player 2");
}

int bench_player()
{
    const int me = process_index();
    console() << "[Bench] waiting on the sideline as branch " << me << "\n";

    std::array<int, 3> roster{me, 0, 0};
    size_t roster_size = 1;
    std::atomic<bool> leave_requested{false};
    std::mutex leave_mutex;
    std::condition_variable leave_cv;

    activate_slot([&](UpdateRoster update) {
        roster_size = static_cast<size_t>(update.count);
        for (size_t idx = 0; idx < roster_size; ++idx) {
            roster[idx] = update.players[idx];
        }
        console() << "[Bench] roster updated to "
                  << roster_to_string(roster, roster_size) << "\n";
    });

    activate_slot([&](LeaveSwarm leave) {
        if (leave.target != me || leave_requested.exchange(true)) {
            return;
        }
        console() << "[Bench] received invitation to exit\n";
        world() << BenchDeparted{me};
        leave_cv.notify_one();
    });

    activate_slot([&](StopPlay) {
        if (!leave_requested.exchange(true)) {
            world() << BenchDeparted{me};
        }
        leave_cv.notify_one();
    });

    activate_slot([&](Ball ball) {
        if (ball.target != me || leave_requested.load(std::memory_order_acquire)) {
            return;
        }

        const auto next = next_target(roster, roster_size, me);
        const auto next_hit = ball.hit + 1;

        console() << "[Bench] hit " << next_hit
                  << ", sending to player " << next << "\n";
        world() << HitReport{me, static_cast<int>(roster_size), next_hit};
        world() << Ball{next, next_hit};
    });

    world() << BenchReady{me};

    std::unique_lock<std::mutex> lk(leave_mutex);
    leave_cv.wait(lk, [&] { return leave_requested.load(std::memory_order_acquire); });

    deactivate_all_slots();
    console() << "[Bench] leaving the swarm\n";
    return 0;
}

int controller()
{
    console() << "[Controller] Dynamic ping-pong demo starting\n";

    std::mutex state_mutex;
    std::condition_variable done_cv;
    bool started = false;
    bool done = false;
    int ready_players = 0;
    int players_stopped = 0;

    uint64_t total_hits = 0;
    uint64_t hits_since_invite = 0;
    uint64_t hits_in_cycle = 0;
    uint64_t hits_after_completion = 0;
    int completed_cycles = 0;
    bool bench_active = false;
    bool bench_spawning = false;
    bool bench_retiring = false;
    bool stop_sent = false;

    constexpr uint64_t warmup_initial = 10;
    constexpr uint64_t warmup_between = 6;
    constexpr uint64_t three_way_hits = 12;
    constexpr uint64_t cooldown_hits = 6;
    constexpr int target_cycles = 2;

    activate_slot([&](PlayerReady ready) {
        bool launch = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!started) {
                ++ready_players;
                if (ready_players == 2) {
                    started = true;
                    launch = true;
                }
            }
        }
        console() << "[Controller] player " << ready.index << " is ready\n";
        if (launch) {
            console() << "[Controller] warm-up rally begins\n";
            world() << make_roster({1, 2});
            world() << Ball{1, 0};
        }
    });

    activate_slot([&](HitReport report) {
        bool request_spawn = false;
        bool request_leave = false;
        bool send_two_player_roster = false;
        bool request_stop = false;
        uint64_t hit_number = 0;
        uint64_t cycle_hits = 0;

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ++total_hits;
            hit_number = total_hits;

            if (report.roster_size > 2) {
                ++hits_in_cycle;
                cycle_hits = hits_in_cycle;
                if (!bench_retiring && hits_in_cycle >= three_way_hits) {
                    bench_retiring = true;
                    request_leave = true;
                    send_two_player_roster = true;
                }
            }
            else {
                if (!bench_active && !bench_spawning && completed_cycles < target_cycles) {
                    ++hits_since_invite;
                    const auto threshold = (completed_cycles == 0)
                        ? warmup_initial
                        : warmup_between;
                    if (hits_since_invite >= threshold) {
                        bench_spawning = true;
                        hits_since_invite = 0;
                        request_spawn = true;
                    }
                }
                else if (completed_cycles >= target_cycles && !bench_spawning && !bench_active) {
                    ++hits_after_completion;
                    if (!stop_sent && hits_after_completion >= cooldown_hits) {
                        stop_sent = true;
                        request_stop = true;
                    }
                }

                if (bench_active || bench_spawning) {
                    hits_since_invite = 0;
                }
            }
        }

        if (request_spawn) {
            console() << "[Controller] inviting bench player after "
                      << hit_number << " hits\n";
            if (spawn_branch(bench_branch_index) == 0) {
                console() << "[Controller] failed to spawn bench player\n";
                std::lock_guard<std::mutex> lock(state_mutex);
                bench_spawning = false;
            }
        }

        if (send_two_player_roster) {
            console() << "[Controller] rotating bench out after "
                      << cycle_hits << " three-player hits\n";
            world() << make_roster({1, 2});
        }

        if (request_leave) {
            world() << LeaveSwarm{bench_branch_index};
        }

        if (request_stop) {
            console() << "[Controller] stopping rally after cooldown\n";
            world() << StopPlay{};
        }
    });

    activate_slot([&](BenchReady ready) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            bench_active = true;
            bench_spawning = false;
            bench_retiring = false;
            hits_in_cycle = 0;
            hits_since_invite = 0;
            hits_after_completion = 0;
        }
        console() << "[Controller] bench player " << ready.index << " joined\n";
        world() << make_roster({1, 2, ready.index});
    });

    activate_slot([&](BenchDeparted departed) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            bench_active = false;
            bench_retiring = false;
            bench_spawning = false;
            ++completed_cycles;
            hits_in_cycle = 0;
            hits_since_invite = 0;
            hits_after_completion = 0;
        }
        console() << "[Controller] bench player " << departed.index
                  << " completed a cycle\n";
    });

    activate_slot([&](PlayerStopped stopped) {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ++players_stopped;
            if (players_stopped == 2) {
                done = true;
                notify = true;
            }
        }
        console() << "[Controller] player " << stopped.index << " stopped\n";
        if (notify) {
            done_cv.notify_one();
        }
    });

    std::unique_lock<std::mutex> lk(state_mutex);
    done_cv.wait(lk, [&] { return done; });
    lk.unlock();

    deactivate_all_slots();
    console() << "[Controller] dynamic ping-pong complete\n";
    barrier("dynamic-pingpong-finished");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    init(
        argc,
        argv,
        Process_descriptor(player_one),
        Process_descriptor(player_two),
        Process_descriptor(bench_player, /*auto_start*/ false),
        Process_descriptor(controller)
    );

    if (process_index() == 0) {
        console() << "\nDynamic swarm demonstration finished.\n";
    }

    finalize();
    return 0;
}

