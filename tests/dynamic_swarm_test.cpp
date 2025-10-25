//
// Sintra Dynamic Swarm Test
//
// This test is modeled after example_6 (dynamic swarm). It validates that
// processes can dynamically invite and remove additional participants during
// an ongoing exchange. The controller should invite the bench player twice,
// collect a fixed number of three-player hits, and then return to the two
// baseline players before shutting the rally down.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr int kBenchBranchIndex = 3;
constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

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

struct ControllerStats {
    std::vector<std::uint64_t> hits_at_invite;
    std::vector<std::uint64_t> hits_per_cycle;
    bool stop_sent = false;
    bool bench_spawn_failed = false;
    int bench_ready_count = 0;
    int bench_depart_count = 0;
    int players_stopped = 0;
    std::uint64_t total_hits = 0;
    std::uint64_t hits_after_completion = 0;
};

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

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value) {
        return std::filesystem::path(value);
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_tests";
    std::filesystem::create_directories(base);

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "dynamic_swarm_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_results(const std::filesystem::path& dir, const ControllerStats& stats)
{
    std::ofstream out(dir / "dynamic_swarm_results.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open dynamic_swarm_results.txt for writing");
    }

    out << stats.hits_at_invite.size() << ' ' << stats.bench_ready_count << ' '
        << stats.bench_depart_count << ' ' << stats.players_stopped << ' '
        << (stats.stop_sent ? 1 : 0) << ' ' << (stats.bench_spawn_failed ? 1 : 0) << '\n';

    out << stats.hits_per_cycle.size();
    for (auto value : stats.hits_per_cycle) {
        out << ' ' << value;
    }
    out << '\n';

    out << stats.hits_at_invite.size();
    for (auto value : stats.hits_at_invite) {
        out << ' ' << value;
    }
    out << '\n';

    out << stats.total_hits << ' ' << stats.hits_after_completion << '\n';
}

int run_regular_player()
{
    const int me = sintra::process_index();

    std::array<int, 3> roster{1, 2, 0};
    size_t roster_size = 2;
    std::atomic<bool> stop_requested{false};
    std::mutex stop_mutex;
    std::condition_variable stop_cv;

    sintra::activate_slot([&](UpdateRoster update) {
        roster_size = static_cast<size_t>(update.count);
        for (size_t idx = 0; idx < roster_size; ++idx) {
            roster[idx] = update.players[idx];
        }
    });

    sintra::activate_slot([&](StopPlay) {
        stop_requested.store(true, std::memory_order_release);
        stop_cv.notify_one();
    });

    sintra::activate_slot([&](Ball ball) {
        if (ball.target != me) {
            return;
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            return;
        }

        const auto next = next_target(roster, roster_size, me);
        const auto next_hit = ball.hit + 1;

        sintra::world() << HitReport{me, static_cast<int>(roster_size), next_hit};
        sintra::world() << Ball{next, next_hit};
    });

    sintra::world() << PlayerReady{me};

    std::unique_lock<std::mutex> lk(stop_mutex);
    stop_cv.wait(lk, [&] { return stop_requested.load(std::memory_order_acquire); });

    sintra::deactivate_all_slots();
    sintra::world() << PlayerStopped{me};

    sintra::barrier("dynamic-pingpong-finished");
    return 0;
}

int player_one()
{
    return run_regular_player();
}

int player_two()
{
    return run_regular_player();
}

int bench_player()
{
    const int me = sintra::process_index();

    std::array<int, 3> roster{me, 0, 0};
    size_t roster_size = 1;
    std::atomic<bool> leave_requested{false};
    std::mutex leave_mutex;
    std::condition_variable leave_cv;

    sintra::activate_slot([&](UpdateRoster update) {
        roster_size = static_cast<size_t>(update.count);
        for (size_t idx = 0; idx < roster_size; ++idx) {
            roster[idx] = update.players[idx];
        }
    });

    sintra::activate_slot([&](LeaveSwarm leave) {
        if (leave.target != me || leave_requested.exchange(true)) {
            return;
        }
        sintra::world() << BenchDeparted{me};
        leave_cv.notify_one();
    });

    sintra::activate_slot([&](StopPlay) {
        if (!leave_requested.exchange(true)) {
            sintra::world() << BenchDeparted{me};
        }
        leave_cv.notify_one();
    });

    sintra::activate_slot([&](Ball ball) {
        if (ball.target != me || leave_requested.load(std::memory_order_acquire)) {
            return;
        }

        const auto next = next_target(roster, roster_size, me);
        const auto next_hit = ball.hit + 1;

        sintra::world() << HitReport{me, static_cast<int>(roster_size), next_hit};
        sintra::world() << Ball{next, next_hit};
    });

    sintra::world() << BenchReady{me};

    std::unique_lock<std::mutex> lk(leave_mutex);
    leave_cv.wait(lk, [&] { return leave_requested.load(std::memory_order_acquire); });

    sintra::deactivate_all_slots();
    return 0;
}

int controller()
{
    ControllerStats stats;

    std::mutex state_mutex;
    std::condition_variable done_cv;
    bool started = false;
    bool done = false;
    int ready_players = 0;

    int players_stopped = 0;

    std::uint64_t total_hits = 0;
    std::uint64_t hits_since_invite = 0;
    std::uint64_t hits_in_cycle = 0;
    std::uint64_t hits_after_completion = 0;
    int completed_cycles = 0;
    bool bench_active = false;
    bool bench_spawning = false;
    bool bench_retiring = false;
    bool stop_sent = false;

    constexpr std::uint64_t warmup_initial = 10;
    constexpr std::uint64_t warmup_between = 6;
    constexpr std::uint64_t three_way_hits = 12;
    constexpr std::uint64_t cooldown_hits = 6;
    constexpr int target_cycles = 2;

    sintra::activate_slot([&](PlayerReady ready) {
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
        if (launch) {
            sintra::world() << make_roster({1, 2});
            sintra::world() << Ball{1, 0};
        }
    });

    sintra::activate_slot([&](HitReport report) {
        bool request_spawn = false;
        bool request_leave = false;
        bool send_two_player_roster = false;
        bool request_stop = false;
        std::uint64_t hit_number = 0;
        std::uint64_t cycle_hits = 0;

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
                        stats.hits_at_invite.push_back(hit_number);
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
            if (sintra::spawn_branch(kBenchBranchIndex) == 0) {
                std::lock_guard<std::mutex> lock(state_mutex);
                bench_spawning = false;
                stats.bench_spawn_failed = true;
            }
        }

        if (send_two_player_roster) {
            sintra::world() << make_roster({1, 2});
        }

        if (request_leave) {
            sintra::world() << LeaveSwarm{kBenchBranchIndex};
        }

        if (request_stop) {
            sintra::world() << StopPlay{};
            stats.stop_sent = true;
        }
    });

    sintra::activate_slot([&](BenchReady ready) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            bench_active = true;
            bench_spawning = false;
            bench_retiring = false;
            hits_in_cycle = 0;
            hits_since_invite = 0;
            hits_after_completion = 0;
            ++stats.bench_ready_count;
        }
        sintra::world() << make_roster({1, 2, ready.index});
    });

    sintra::activate_slot([&](BenchDeparted) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            bench_active = false;
            bench_retiring = false;
            bench_spawning = false;
            ++completed_cycles;
            stats.hits_per_cycle.push_back(hits_in_cycle);
            hits_in_cycle = 0;
            hits_since_invite = 0;
            hits_after_completion = 0;
            ++stats.bench_depart_count;
        }
    });

    sintra::activate_slot([&](PlayerStopped) {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ++players_stopped;
            ++stats.players_stopped;
            if (players_stopped == 2) {
                done = true;
                notify = true;
                stats.total_hits = total_hits;
                stats.hits_after_completion = hits_after_completion;
            }
        }
        if (notify) {
            done_cv.notify_one();
        }
    });

    std::unique_lock<std::mutex> lk(state_mutex);
    done_cv.wait(lk, [&] { return done; });
    lk.unlock();

    sintra::deactivate_all_slots();

    const auto shared_dir = get_shared_directory();
    write_results(shared_dir, stats);

    sintra::barrier("dynamic-pingpong-finished");
    return 0;
}

int parse_expectation_file(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return 1;
    }

    std::size_t invites = 0;
    int bench_ready_count = 0;
    int bench_depart_count = 0;
    int players_stopped = 0;
    int stop_sent_flag = 0;
    int spawn_failed_flag = 0;

    if (!(in >> invites >> bench_ready_count >> bench_depart_count >> players_stopped >> stop_sent_flag >> spawn_failed_flag)) {
        return 1;
    }

    std::size_t cycle_count = 0;
    if (!(in >> cycle_count)) {
        return 1;
    }
    std::vector<std::uint64_t> cycle_hits(cycle_count);
    for (std::size_t i = 0; i < cycle_count; ++i) {
        if (!(in >> cycle_hits[i])) {
            return 1;
        }
    }

    std::size_t invite_count_copy = 0;
    if (!(in >> invite_count_copy)) {
        return 1;
    }
    std::vector<std::uint64_t> invite_hits(invite_count_copy);
    for (std::size_t i = 0; i < invite_count_copy; ++i) {
        if (!(in >> invite_hits[i])) {
            return 1;
        }
    }

    std::uint64_t total_hits = 0;
    std::uint64_t cooldown_hits = 0;
    if (!(in >> total_hits >> cooldown_hits)) {
        return 1;
    }

    if (invites != 2 || bench_ready_count != 2 || bench_depart_count != 2) {
        return 1;
    }
    if (players_stopped != 2 || stop_sent_flag != 1 || spawn_failed_flag != 0) {
        return 1;
    }
    if (cycle_hits.size() != 2) {
        return 1;
    }
    for (auto value : cycle_hits) {
        if (value != 12) {
            return 1;
        }
    }
    if (invite_hits.size() != 2) {
        return 1;
    }
    if (invite_hits[0] < 10) {
        return 1;
    }
    if (invite_hits[1] <= invite_hits[0]) {
        return 1;
    }
    if (cooldown_hits < 6) {
        return 1;
    }

    (void)total_hits;
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    const auto shared_dir = is_spawned ? get_shared_directory() : ensure_shared_directory();
    const auto results_path = shared_dir / "dynamic_swarm_results.txt";

    if (!is_spawned) {
        std::error_code ec;
        std::filesystem::remove(results_path, ec);
    }

    sintra::init(
        argc,
        argv,
        sintra::Process_descriptor(player_one),
        sintra::Process_descriptor(player_two),
        sintra::Process_descriptor(bench_player, /*auto_start*/ false),
        sintra::Process_descriptor(controller)
    );

    sintra::finalize();

    if (!is_spawned) {
        const int status = parse_expectation_file(results_path);
        try {
            std::filesystem::remove_all(shared_dir);
        }
        catch (...) {
        }
        return status;
    }

    return 0;
}

