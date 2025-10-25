//
// Sintra library, example 1
//
// Dynamic swarm membership ping-pong demonstration.
//
// This example showcases how processes can join and leave an existing Sintra
// swarm at runtime.  A coordinator process bootstraps the swarm, launches the
// initial "ping" and "pong" participants, and then dynamically spawns a relay
// process that joins the ping-pong for a few exchanges before departing.  After
// another burst of two-way traffic, the relay is spawned again to re-enter the
// conversation.  Finally, all processes are shut down cleanly.
//
// The example highlights three capabilities:
//   * Launching additional processes after the swarm has already started by
//     invoking `sintra::spawn_swarm_process`.
//   * Orchestrating the behaviour of the running processes via control
//     messages so that they seamlessly adapt when new peers arrive or leave.
//   * Gracefully draining and terminating processes while the remaining swarm
//     continues operating.
//
// Roles:
//   - Coordinator: controls process lifecycle and monitors exchange counts.
//   - Ping: drives the ping/pong exchanges.
//   - Pong: responds to ping messages.
//   - Relay: participates in the three-way ping pong when requested.
//
// Build and run the example normally.  Only the coordinator is started by the
// user; the remaining processes are spawned on demand.
//

#include <sintra/sintra.h>

#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace std;
using namespace sintra;

namespace {

enum class Role : uint8_t {
    Coordinator = 0,
    Ping,
    Pong,
    Relay
};

const char* role_name(Role role)
{
    switch (role) {
    case Role::Coordinator: return "coordinator";
    case Role::Ping:        return "ping";
    case Role::Pong:        return "pong";
    case Role::Relay:       return "relay";
    }
    return "unknown";
}

struct Ping
{
    uint64_t sequence;
};

struct Pong
{
    uint64_t sequence;
};

struct Relay_kickoff
{
    uint64_t sequence;
};

struct Start_two_way
{
    uint64_t next_sequence;
};

struct Use_relay {};
struct Stop_relay {};
struct Shutdown {};
struct Leave_relay {};

struct Role_ready
{
    Role role;
};

struct Role_finished
{
    Role role;
};

struct Exchange_complete
{
    uint64_t sequence;
};

constexpr uint64_t k_two_way_before_first_relay = 6;
constexpr uint64_t k_three_way_cycles_per_round = 3;
constexpr uint64_t k_two_way_between_relays = 4;
constexpr uint32_t k_relay_rounds = 2;
constexpr uint64_t k_two_way_after_final_round = 4;

const uint64_t k_total_target_exchanges =
    k_two_way_before_first_relay +
    (k_three_way_cycles_per_round * k_relay_rounds) +
    (k_two_way_between_relays * (k_relay_rounds - 1)) +
    k_two_way_after_final_round;

string g_program_path;

Role parse_role(int argc, char* const argv[])
{
    for (int i = 1; i < argc; ++i) {
        string_view arg(argv[i]);
        if (arg == "--role" && i + 1 < argc) {
            ++i;
            arg = argv[i];
        }
        else if (arg.rfind("--role=", 0) == 0) {
            arg.remove_prefix(7);
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
        if (arg == "relay") {
            return Role::Relay;
        }
        return Role::Coordinator;
    }

    return Role::Coordinator;
}

vector<string> spawn_args_for(Role role)
{
    return {g_program_path, string("--role=") + role_name(role)};
}

bool spawn_role(Role role)
{
    const auto args = spawn_args_for(role);
    const auto spawned = spawn_swarm_process(g_program_path, args);
    if (spawned != 1) {
        console() << "[Coordinator] Failed to spawn " << role_name(role) << " process\n";
        return false;
    }
    return true;
}

int run_ping()
{
    console() << "[Ping] Starting and waiting for coordinator instructions\n";

    mutex m;
    condition_variable cv;
    bool exit_requested = false;
    bool notified_finish = false;
    bool use_relay = false;
    bool relay_available = false;
    uint64_t next_sequence = 0;

    auto send_next = [&](uint64_t sequence, bool to_relay) {
        if (to_relay) {
            world() << Relay_kickoff{sequence};
        }
        else {
            world() << Ping{sequence};
        }
    };

    activate_slot([&](Start_two_way start) {
        bool to_relay = false;
        uint64_t sequence = 0;
        {
            lock_guard<mutex> lk(m);
            next_sequence = start.next_sequence;
            sequence = next_sequence;
            to_relay = use_relay && relay_available;
        }
        send_next(sequence, to_relay);
    });

    activate_slot([&](Use_relay) {
        lock_guard<mutex> lk(m);
        use_relay = true;
    });

    activate_slot([&](Stop_relay) {
        lock_guard<mutex> lk(m);
        use_relay = false;
        relay_available = false;
    });

    activate_slot([&](Role_ready ready) {
        if (ready.role != Role::Relay) {
            return;
        }
        lock_guard<mutex> lk(m);
        relay_available = true;
    });

    activate_slot([&](Role_finished finished) {
        if (finished.role != Role::Relay) {
            return;
        }
        lock_guard<mutex> lk(m);
        relay_available = false;
    });

    activate_slot([&](Pong pong_msg) {
        uint64_t to_send = 0;
        bool send_to_relay = false;
        {
            lock_guard<mutex> lk(m);
            next_sequence = pong_msg.sequence + 1;
            to_send = next_sequence;
            send_to_relay = use_relay && relay_available;
        }
        world() << Exchange_complete{pong_msg.sequence};
        send_next(to_send, send_to_relay);
    });

    activate_slot([&](Shutdown) {
        unique_lock<mutex> lk(m);
        if (!notified_finish) {
            notified_finish = true;
            world() << Role_finished{Role::Ping};
        }
        exit_requested = true;
        lk.unlock();
        cv.notify_one();
    });

    world() << Role_ready{Role::Ping};

    unique_lock<mutex> lk(m);
    cv.wait(lk, [&]{ return exit_requested; });

    deactivate_all_slots();
    console() << "[Ping] Shutting down\n";
    return 0;
}

int run_pong()
{
    console() << "[Pong] Starting and waiting for pings\n";

    mutex m;
    condition_variable cv;
    bool exit_requested = false;
    bool notified_finish = false;

    activate_slot([&](Ping ping_msg) {
        world() << Pong{ping_msg.sequence};
    });

    activate_slot([&](Shutdown) {
        unique_lock<mutex> lk(m);
        if (!notified_finish) {
            notified_finish = true;
            world() << Role_finished{Role::Pong};
        }
        exit_requested = true;
        lk.unlock();
        cv.notify_one();
    });

    world() << Role_ready{Role::Pong};

    unique_lock<mutex> lk(m);
    cv.wait(lk, [&]{ return exit_requested; });

    deactivate_all_slots();
    console() << "[Pong] Shutting down\n";
    return 0;
}

int run_relay()
{
    console() << "[Relay] Spawned and waiting for kickoff messages\n";

    mutex m;
    condition_variable cv;
    bool exit_requested = false;
    bool active = false;

    activate_slot([&](Use_relay) {
        lock_guard<mutex> lk(m);
        active = true;
    });

    activate_slot([&](Stop_relay) {
        lock_guard<mutex> lk(m);
        active = false;
    });

    activate_slot([&](Relay_kickoff kickoff) {
        bool should_forward = false;
        {
            lock_guard<mutex> lk(m);
            should_forward = active;
        }
        if (should_forward) {
            world() << Ping{kickoff.sequence};
        }
    });

    activate_slot([&](Leave_relay) {
        unique_lock<mutex> lk(m);
        exit_requested = true;
        lk.unlock();
        cv.notify_one();
    });

    activate_slot([&](Shutdown) {
        unique_lock<mutex> lk(m);
        exit_requested = true;
        lk.unlock();
        cv.notify_one();
    });

    world() << Role_ready{Role::Relay};

    unique_lock<mutex> lk(m);
    cv.wait(lk, [&]{ return exit_requested; });
    lk.unlock();

    world() << Role_finished{Role::Relay};
    deactivate_all_slots();
    console() << "[Relay] Exiting\n";
    return 0;
}

int run_coordinator()
{
    console() << "[Coordinator] Starting dynamic ping-pong demonstration\n";

    struct State {
        mutex m;
        condition_variable cv;
        bool ping_ready = false;
        bool pong_ready = false;
        bool ping_finished = false;
        bool pong_finished = false;
        bool relay_present = false;
        bool relay_spawning = false;
        bool relay_active = false;
        bool pending_activation = false;
        bool shutdown_initiated = false;
        uint32_t relay_rounds_completed = 0;
        uint64_t total_exchanges = 0;
        uint64_t relay_exchange_count = 0;
        uint64_t next_activation_threshold = k_two_way_before_first_relay;
    } state;

    auto spawn_initial_role = [&](Role role) {
        if (spawn_role(role)) {
            console() << "[Coordinator] Spawned " << role_name(role) << " process\n";
        }
    };

    activate_slot([&](Role_ready ready) {
        bool notify = false;
        bool activate_relay = false;

        {
            lock_guard<mutex> lk(state.m);
            switch (ready.role) {
            case Role::Ping:
                if (!state.ping_ready) {
                    console() << "[Coordinator] Ping process ready\n";
                }
                state.ping_ready = true;
                notify = true;
                break;
            case Role::Pong:
                if (!state.pong_ready) {
                    console() << "[Coordinator] Pong process ready\n";
                }
                state.pong_ready = true;
                notify = true;
                break;
            case Role::Relay:
                state.relay_present = true;
                state.relay_spawning = false;
                if (state.pending_activation) {
                    state.pending_activation = false;
                    state.relay_active = true;
                    state.relay_exchange_count = 0;
                    activate_relay = true;
                }
                notify = true;
                break;
            default:
                break;
            }
        }

        if (activate_relay) {
            console() << "[Coordinator] Relay joined – switching to three-way mode\n";
            world() << Use_relay{};
        }

        if (notify) {
            state.cv.notify_all();
        }
    });

    activate_slot([&](Role_finished finished) {
        lock_guard<mutex> lk(state.m);
        switch (finished.role) {
        case Role::Ping:
            state.ping_finished = true;
            console() << "[Coordinator] Ping process confirmed shutdown\n";
            break;
        case Role::Pong:
            state.pong_finished = true;
            console() << "[Coordinator] Pong process confirmed shutdown\n";
            break;
        case Role::Relay:
            state.relay_present = false;
            state.relay_active = false;
            console() << "[Coordinator] Relay process exited\n";
            break;
        default:
            break;
        }
        state.cv.notify_all();
    });

    activate_slot([&](Exchange_complete exchange) {
        bool spawn_relay_now = false;
        bool stop_relay_now = false;
        bool issue_shutdown = false;

        {
            lock_guard<mutex> lk(state.m);
            ++state.total_exchanges;

            if (state.relay_active) {
                ++state.relay_exchange_count;
                if (state.relay_exchange_count >= k_three_way_cycles_per_round) {
                    stop_relay_now = true;
                    state.relay_active = false;
                    state.relay_exchange_count = 0;
                    ++state.relay_rounds_completed;
                    if (state.relay_rounds_completed < k_relay_rounds) {
                        state.next_activation_threshold =
                            state.total_exchanges + k_two_way_between_relays;
                    }
                    else {
                        state.next_activation_threshold =
                            numeric_limits<uint64_t>::max();
                    }
                }
            }
            else if (state.relay_rounds_completed < k_relay_rounds &&
                     !state.relay_spawning &&
                     !state.relay_present &&
                     state.total_exchanges >= state.next_activation_threshold) {
                state.relay_spawning = true;
                state.pending_activation = true;
                spawn_relay_now = true;
            }

            if (!state.shutdown_initiated &&
                state.relay_rounds_completed >= k_relay_rounds &&
                state.total_exchanges >= k_total_target_exchanges &&
                !state.relay_spawning &&
                !state.relay_present) {
                state.shutdown_initiated = true;
                issue_shutdown = true;
            }
        }

        if (spawn_relay_now) {
            console() << "[Coordinator] Spawning relay process after "
                      << exchange.sequence + 1 << " exchanges\n";
            if (!spawn_role(Role::Relay)) {
                lock_guard<mutex> lk(state.m);
                state.relay_spawning = false;
                state.pending_activation = false;
            }
        }

        if (stop_relay_now) {
            console() << "[Coordinator] Stopping relay after three-way cycle\n";
            world() << Stop_relay{};
            world() << Leave_relay{};
        }

        if (issue_shutdown) {
            console() << "[Coordinator] Target reached – shutting everything down\n";
            world() << Shutdown{};
        }
    });

    spawn_initial_role(Role::Ping);
    spawn_initial_role(Role::Pong);

    {
        unique_lock<mutex> lk(state.m);
        state.cv.wait(lk, [&]{ return state.ping_ready && state.pong_ready; });
    }

    console() << "[Coordinator] Both ping and pong ready – starting two-way ping pong\n";
    world() << Start_two_way{0};

    {
        unique_lock<mutex> lk(state.m);
        state.cv.wait(lk, [&]{
            return state.shutdown_initiated &&
                   state.ping_finished &&
                   state.pong_finished &&
                   !state.relay_present &&
                   !state.relay_spawning;
        });
    }

    console() << "[Coordinator] Demonstration complete\n";
    deactivate_all_slots();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    g_program_path = argv[0];
    const auto role = parse_role(argc, argv);

    init(argc, argv);

    int return_code = 0;
    switch (role) {
    case Role::Coordinator:
        return_code = run_coordinator();
        break;
    case Role::Ping:
        return_code = run_ping();
        break;
    case Role::Pong:
        return_code = run_pong();
        break;
    case Role::Relay:
        return_code = run_relay();
        break;
    }

    finalize();
    return return_code;
}

