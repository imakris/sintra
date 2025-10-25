//
// Sintra Dynamic Swarm Test (modeled after example 6)
//
// This test verifies that processes can dynamically join and leave a
// coordinator-managed swarm at runtime. It exercises spawning an additional
// player, integrating it into an existing ping-pong rally, requesting a
// graceful departure, and returning to the original rotation.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using namespace sintra;

namespace {

constexpr std::size_t kMaxPlayers = 3;

struct Ball
{
    uint64_t volley;
    uint32_t rotation_id;
    std::array<instance_id_type, kMaxPlayers> rotation;
    uint8_t participant_count;
    uint8_t next_index;
};

struct Depart
{
    instance_id_type target;
};

struct PlayerReady
{
    instance_id_type player;
};

struct PlayerDeparted
{
    instance_id_type player;
};

struct Stop
{};

instance_id_type my_process_id()
{
    return runtime_state::instance().managed_process_id();
}

int player_process()
{
    std::mutex shutdown_mutex;
    std::condition_variable shutdown_cv;
    bool should_exit = false;

    std::atomic<uint32_t> current_rotation_id{0};

    activate_slot([&](const Stop&)
    {
        barrier("dynamic-swarm-test-finished", "_sintra_all_processes");
        finalize();
        {
            std::lock_guard<std::mutex> lk(shutdown_mutex);
            should_exit = true;
        }
        shutdown_cv.notify_one();
    });

    activate_slot([&](const Depart& msg)
    {
        if (msg.target != my_process_id()) {
            return;
        }

        world() << PlayerDeparted{my_process_id()};
        finalize();
        {
            std::lock_guard<std::mutex> lk(shutdown_mutex);
            should_exit = true;
        }
        shutdown_cv.notify_one();
    });

    activate_slot([&](const Ball& ball)
    {
        if (ball.participant_count == 0 || ball.participant_count > kMaxPlayers) {
            return;
        }

        const auto observed_id = current_rotation_id.load(std::memory_order_acquire);
        if (ball.rotation_id < observed_id) {
            return;
        }

        if (ball.rotation_id > observed_id) {
            current_rotation_id.store(ball.rotation_id, std::memory_order_release);
        }

        if (ball.next_index >= ball.participant_count || ball.rotation[ball.next_index] != my_process_id()) {
            return;
        }

        Ball next_ball = ball;
        next_ball.volley += 1;
        next_ball.next_index = static_cast<uint8_t>((next_ball.next_index + 1) % next_ball.participant_count);
        world() << next_ball;
    });

    world() << PlayerReady{my_process_id()};

    std::unique_lock<std::mutex> lk(shutdown_mutex);
    shutdown_cv.wait(lk, [&]{ return should_exit; });

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    Process_descriptor player_a(player_process);
    Process_descriptor player_b(player_process);
    Process_descriptor extra_player(player_process);
    extra_player.spawn_on_init = false;

    init(argc, argv, player_a, player_b, extra_player);

    bool dynamic_joined = false;
    bool dynamic_departed = false;
    bool spawn_failed = false;

    if (process_index() == 0) {
        std::mutex state_mutex;
        std::condition_variable done_cv;
        bool demonstration_complete = false;

        struct Coordinator_state
        {
            std::vector<instance_id_type> base_players;
            std::vector<instance_id_type> current_rotation;
            instance_id_type dynamic_player = invalid_instance_id;
            bool ball_active = false;
            bool dynamic_active = false;
            bool awaiting_dynamic_ready = false;
            bool awaiting_departure = false;
            bool final_phase = false;
            uint32_t rotation_id = 0;
            uint64_t volley_count = 0;
            int entries_remaining = 1;
        } state;

        const uint64_t two_player_threshold = 4;
        const uint64_t three_player_threshold = 3;
        const uint64_t final_two_player_threshold = 2;

        auto start_rotation = [&](std::vector<instance_id_type> rotation, const std::string& description)
        {
            state.current_rotation = std::move(rotation);
            state.rotation_id += 1;
            state.volley_count = 0;
            state.ball_active = true;
            state.awaiting_departure = false;

            Ball ball{};
            ball.volley = 0;
            ball.rotation_id = state.rotation_id;
            ball.rotation.fill(invalid_instance_id);
            const auto count = static_cast<uint8_t>(std::min<std::size_t>(state.current_rotation.size(), kMaxPlayers));
            ball.participant_count = count;
            for (std::size_t i = 0; i < count; ++i) {
                ball.rotation[i] = state.current_rotation[i];
            }
            ball.next_index = 0;
            console() << description << "\n";
            world() << std::move(ball);
        };

        activate_slot([&](const PlayerReady& msg)
        {
            std::unique_lock<std::mutex> lk(state_mutex);

            if (state.awaiting_dynamic_ready && state.dynamic_player == msg.player) {
                state.dynamic_active = true;
                state.awaiting_dynamic_ready = false;
                dynamic_joined = true;

                auto rotation = state.base_players;
                rotation.push_back(msg.player);
                lk.unlock();
                start_rotation(std::move(rotation), "Three-player rally begins");
                return;
            }

            if (std::find(state.base_players.begin(), state.base_players.end(), msg.player) == state.base_players.end()) {
                state.base_players.push_back(msg.player);
                std::sort(state.base_players.begin(), state.base_players.end());
            }

            if (!state.ball_active && state.base_players.size() >= 2) {
                auto rotation = state.base_players;
                lk.unlock();
                start_rotation(std::move(rotation), "Two-player rally begins");
            }
        });

        activate_slot([&](const PlayerDeparted& msg)
        {
            std::unique_lock<std::mutex> lk(state_mutex);
            if (msg.player != state.dynamic_player) {
                return;
            }

            console() << "Extra player " << msg.player << " has left the rally\n";
            state.dynamic_active = false;
            state.awaiting_departure = false;
            state.dynamic_player = invalid_instance_id;
            dynamic_departed = true;

            auto rotation = state.base_players;
            lk.unlock();
            start_rotation(std::move(rotation), "Returning to two-player rally");
        });

        activate_slot([&](const Ball& ball)
        {
            std::unique_lock<std::mutex> lk(state_mutex);
            if (ball.rotation_id != state.rotation_id) {
                return;
            }

            state.volley_count += 1;

            if (state.dynamic_active) {
                if (!state.awaiting_departure && state.volley_count >= three_player_threshold) {
                    if (state.dynamic_player != invalid_instance_id) {
                        console() << "Requesting extra player " << state.dynamic_player
                                   << " to exit after " << state.volley_count << " volleys\n";
                        world() << Depart{state.dynamic_player};
                        state.awaiting_departure = true;
                    }
                }
                return;
            }

            if (state.awaiting_dynamic_ready) {
                return;
            }

            if (state.entries_remaining > 0 && state.volley_count >= two_player_threshold) {
                lk.unlock();
                auto spawned = spawn_branch(3);
                lk.lock();
                if (spawned != invalid_instance_id) {
                    console() << "Inviting extra player " << spawned << " into the rally\n";
                    state.dynamic_player = spawned;
                    state.awaiting_dynamic_ready = true;
                    state.entries_remaining -= 1;
                }
                else {
                    console() << "Failed to spawn additional player\n";
                    state.entries_remaining = 0;
                    spawn_failed = true;
                    state.final_phase = true;
                    demonstration_complete = true;
                    lk.unlock();
                    world() << Stop{};
                    done_cv.notify_one();
                    return;
                }
                state.volley_count = 0;
                return;
            }

            if (!state.final_phase && state.entries_remaining == 0 && state.volley_count >= final_two_player_threshold) {
                console() << "Dynamic swarm test complete. Stopping rally.\n";
                world() << Stop{};
                state.final_phase = true;
                demonstration_complete = true;
                lk.unlock();
                done_cv.notify_one();
            }
        });

        std::unique_lock<std::mutex> lk(state_mutex);
        done_cv.wait(lk, [&]{ return demonstration_complete; });

        lk.unlock();
        barrier("dynamic-swarm-test-finished", "_sintra_all_processes");
    }

    finalize();

    if (process_index() == 0) {
        return (dynamic_joined && dynamic_departed && !spawn_failed) ? 0 : 1;
    }

    return 0;
}
