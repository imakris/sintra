#include <sintra/sintra.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace {

constexpr int kBenchBranchIndex = 2;
constexpr int kTargetHits = 6;

enum class Hitter : std::uint8_t { Player = 0, Bench = 1 };

const char* hitter_name(Hitter hitter)
{
    switch (hitter) {
        case Hitter::Player: return "player";
        case Hitter::Bench:  return "bench";
    }
    return "unknown";
}

struct PlayerReady {};
struct Hit
{
    Hitter from;
    int sequence;
};
struct BenchReady { sintra::instance_id_type bench_iid; };
struct BenchSwingRequest { sintra::instance_id_type target; };
struct BenchSwingAck { sintra::instance_id_type bench_iid; };
struct BenchDepartRequest { sintra::instance_id_type target; };
struct StopAll {};

int player_process()
{
    std::mutex stop_mutex;
    std::condition_variable stop_cv;
    bool should_stop = false;

    sintra::activate_slot([&](StopAll) {
        std::lock_guard<std::mutex> lock(stop_mutex);
        should_stop = true;
        stop_cv.notify_all();
    });

    sintra::world() << PlayerReady{};
    sintra::console() << "[Player] ready\n";

    for (int i = 0; i < kTargetHits; ++i) {
        sintra::world() << Hit{Hitter::Player, i + 1};
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::unique_lock<std::mutex> lock(stop_mutex);
    stop_cv.wait(lock, [&] { return should_stop; });

    return 0;
}

int bench_process()
{
    std::mutex stop_mutex;
    std::condition_variable stop_cv;
    bool should_stop = false;
    const auto self = sintra::runtime_state::instance().managed_process_id();

    sintra::activate_slot([&](const BenchSwingRequest& request) {
        if (request.target != self) {
            return;
        }
        sintra::world() << Hit{Hitter::Bench, 0};
        sintra::world() << BenchSwingAck{self};
        sintra::console() << "[Bench] swing ack sent\n";
    });

    sintra::activate_slot([&](const BenchDepartRequest& request) {
        if (request.target != self) {
            return;
        }
        sintra::console() << "[Bench] depart acknowledged\n";
        std::lock_guard<std::mutex> lock(stop_mutex);
        should_stop = true;
        stop_cv.notify_all();
    });

    sintra::activate_slot([&](StopAll) {
        std::lock_guard<std::mutex> lock(stop_mutex);
        should_stop = true;
        stop_cv.notify_all();
    });

    sintra::world() << BenchReady{self};
    sintra::console() << "[Bench] ready with iid=" << self << "\n";

    std::unique_lock<std::mutex> lock(stop_mutex);
    stop_cv.wait(lock, [&] { return should_stop; });

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::Process_descriptor player_desc(player_process);
    sintra::Process_descriptor bench_desc(bench_process);
    bench_desc.auto_start = false;

    sintra::init(argc, argv, player_desc, bench_desc);

    if (sintra::process_index() == 0) {
        std::mutex state_mutex;
        std::condition_variable state_cv;

        [[maybe_unused]] bool player_ready = false;
        bool bench_spawn_requested = false;
        [[maybe_unused]] bool bench_ready = false;
        bool bench_confirmed = false;
        bool bench_depart_requested = false;
        bool bench_departed = false;
        bool stop_seen = false;
        int hit_count = 0;
        sintra::instance_id_type bench_id = sintra::invalid_instance_id;

        sintra::activate_slot([&](PlayerReady) {
            std::lock_guard<std::mutex> lock(state_mutex);
            player_ready = true;
            (void)player_ready;
        });

        sintra::activate_slot([&](const Hit& hit) {
            sintra::instance_id_type depart_target = sintra::invalid_instance_id;
            bool request_depart = false;
            bool request_spawn = false;

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                ++hit_count;
                sintra::console() << "[Controller] hit " << hit_count
                                   << " from " << hitter_name(hit.from) << "\n";
                if (!bench_spawn_requested && hit_count >= 2) {
                    bench_spawn_requested = true;
                    request_spawn = true;
                }
                if (bench_confirmed && !bench_depart_requested && hit_count >= kTargetHits && bench_id != sintra::invalid_instance_id) {
                    bench_depart_requested = true;
                    depart_target = bench_id;
                    request_depart = true;
                }
                if (bench_departed && hit_count >= kTargetHits) {
                    state_cv.notify_all();
                }
            }

            if (request_spawn) {
                const auto spawned = sintra::spawn_branch(kBenchBranchIndex);
                std::lock_guard<std::mutex> lock(state_mutex);
                if (spawned != sintra::invalid_instance_id) {
                    sintra::console() << "[Controller] spawned bench iid=" << spawned << "\n";
                    bench_id = spawned;
                }
                else {
                    sintra::console() << "[Controller] bench spawn failed, will retry\n";
                    bench_spawn_requested = false;
                }
            }

            if (request_depart && depart_target != sintra::invalid_instance_id) {
                sintra::console() << "[Controller] requesting bench depart iid=" << depart_target << "\n";
                sintra::world() << BenchDepartRequest{depart_target};
            }
        });

        sintra::activate_slot([&](const BenchReady& ready) {
            sintra::instance_id_type swing_target = ready.bench_iid;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                bench_ready = true;
                if (bench_id == sintra::invalid_instance_id) {
                    bench_id = ready.bench_iid;
                }
                swing_target = bench_id;
            }

            if (swing_target != sintra::invalid_instance_id) {
                sintra::console() << "[Controller] bench ready, requesting swing iid=" << swing_target << "\n";
                sintra::world() << BenchSwingRequest{swing_target};
            }
        });

        sintra::activate_slot([&](const BenchSwingAck& ack) {
            sintra::instance_id_type depart_target = sintra::invalid_instance_id;
            bool request_depart = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                bench_confirmed = true;
                sintra::console() << "[Controller] bench swing ack from iid=" << ack.bench_iid << "\n";
                if (bench_id == sintra::invalid_instance_id) {
                    bench_id = ack.bench_iid;
                }
                if (!bench_depart_requested && hit_count >= kTargetHits && bench_id != sintra::invalid_instance_id) {
                    bench_depart_requested = true;
                    depart_target = bench_id;
                    request_depart = true;
                }
            }

            if (request_depart && depart_target != sintra::invalid_instance_id) {
                sintra::console() << "[Controller] confirming depart for iid=" << depart_target << "\n";
                sintra::world() << BenchDepartRequest{depart_target};

                std::thread([&, depart_target] {
                    sintra::console() << "[Controller] removing bench iid=" << depart_target << "\n";
                    const auto coord_id = sintra::runtime_state::instance().coordinator_id();
                    sintra::Coordinator::rpc_remove_process_from_group(
                        coord_id, "_sintra_external_processes", depart_target);

                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        bench_departed = true;
                        bench_id = depart_target;
                        sintra::console() << "[Controller] bench departed iid=" << depart_target << "\n";
                        state_cv.notify_all();
                    }
                }).detach();
            }
        });

        sintra::activate_slot([&](StopAll) {
            std::lock_guard<std::mutex> lock(state_mutex);
            stop_seen = true;
            sintra::console() << "[Controller] stop acknowledged\n";
            state_cv.notify_all();
        });

        {
            std::unique_lock<std::mutex> lock(state_mutex);
            state_cv.wait(lock, [&] { return bench_departed && hit_count >= kTargetHits; });
        }

        sintra::world() << StopAll{};

        {
            std::unique_lock<std::mutex> lock(state_mutex);
            state_cv.wait(lock, [&] { return stop_seen; });
        }

        sintra::barrier("dynamic-swarm-finished", "_sintra_all_processes");
        sintra::finalize();
    }

    return 0;
}
