#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct SwingBarrierSignal {};
struct SwingDepartSignal {};

enum class Role {
    Coordinator,
    Spawner,
    Observer,
    Swing,
};

struct Promise_state {
    std::shared_ptr<std::promise<void>> promise;
    std::shared_ptr<std::atomic<bool>> completed;
};

struct Lifecycle_handlers {
    std::shared_future<void> joined;
    std::shared_future<void> departed;
};

constexpr std::chrono::seconds kEventTimeout{5};
constexpr std::chrono::milliseconds kCleanupGrace{50};

void fulfill_once(const Promise_state& state)
{
    if (!state.completed->exchange(true, std::memory_order_acq_rel)) {
        try {
            state.promise->set_value();
        }
        catch (const std::future_error&) {
        }
    }
}

void fail_once(const Promise_state& state, const std::string& message)
{
    if (!state.completed->exchange(true, std::memory_order_acq_rel)) {
        state.promise->set_exception(std::make_exception_ptr(std::runtime_error(message)));
    }
}

Lifecycle_handlers install_common_handlers()
{
    Promise_state joined{
        std::make_shared<std::promise<void>>(),
        std::make_shared<std::atomic<bool>>(false)
    };
    Promise_state departed{
        std::make_shared<std::promise<void>>(),
        std::make_shared<std::atomic<bool>>(false)
    };

    auto joined_future = joined.promise->get_future().share();
    auto departed_future = departed.promise->get_future().share();

    sintra::activate_slot([joined](const SwingBarrierSignal&) {
        try {
            if (!sintra::barrier("swing-joined", "_sintra_all_processes")) {
                fail_once(joined, "swing-joined barrier failed");
                return;
            }
            fulfill_once(joined);
        }
        catch (...) {
            fail_once(joined, "swing-joined barrier threw");
        }
    });

    sintra::activate_slot([departed](const SwingDepartSignal&) {
        try {
            if (!sintra::barrier("swing-depart", "_sintra_all_processes")) {
                fail_once(departed, "swing-depart barrier failed");
                return;
            }
            fulfill_once(departed);
        }
        catch (...) {
            fail_once(departed, "swing-depart barrier threw");
        }
    });

    return {std::move(joined_future), std::move(departed_future)};
}

Role determine_role(int argc, char* argv[])
{
    std::string role_flag;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--lifecycle-role" && i + 1 < argc) {
            role_flag = argv[++i];
        }
        else if (arg.rfind("--lifecycle-role=", 0) == 0) {
            role_flag = std::string(arg.substr(std::string_view("--lifecycle-role=").size()));
        }
    }

    if (role_flag == "spawner") {
        return Role::Spawner;
    }
    if (role_flag == "observer") {
        return Role::Observer;
    }
    if (role_flag == "swing") {
        return Role::Swing;
    }
    return Role::Coordinator;
}

bool wait_future(const std::shared_future<void>& future)
{
    const auto status = future.wait_for(kEventTimeout);
    if (status != std::future_status::ready) {
        return false;
    }

    try {
        future.get();
    }
    catch (...) {
        return false;
    }

    return true;
}

int run_coordinator(const Lifecycle_handlers& handlers)
{
    if (!sintra::barrier("initial-ready", "_sintra_all_processes")) {
        sintra::finalize();
        return 1;
    }

    if (!wait_future(handlers.joined)) {
        sintra::finalize();
        return 1;
    }

    sintra::world() << SwingDepartSignal{};

    if (!wait_future(handlers.departed)) {
        sintra::finalize();
        return 1;
    }

    std::this_thread::sleep_for(kCleanupGrace);
    const bool cleanup_ok = sintra::barrier("cleanup-ready", "_sintra_all_processes");

    sintra::deactivate_all_slots();
    sintra::finalize();
    return cleanup_ok ? 0 : 1;
}

int run_spawner(const Lifecycle_handlers& handlers, const std::string& binary_path)
{
    if (!sintra::barrier("initial-ready", "_sintra_all_processes")) {
        sintra::finalize();
        return 1;
    }

    const auto spawned = sintra::spawn_swarm_process(binary_path, {"--lifecycle-role=swing"});
    if (spawned != 1) {
        sintra::finalize();
        return 1;
    }

    if (!wait_future(handlers.joined)) {
        sintra::finalize();
        return 1;
    }

    if (!wait_future(handlers.departed)) {
        sintra::finalize();
        return 1;
    }

    std::this_thread::sleep_for(kCleanupGrace);
    const bool cleanup_ok = sintra::barrier("cleanup-ready", "_sintra_all_processes");

    sintra::deactivate_all_slots();
    sintra::finalize();
    return cleanup_ok ? 0 : 1;
}

int run_observer(const Lifecycle_handlers& handlers)
{
    if (!sintra::barrier("initial-ready", "_sintra_all_processes")) {
        sintra::finalize();
        return 1;
    }

    if (!wait_future(handlers.joined)) {
        sintra::finalize();
        return 1;
    }

    if (!wait_future(handlers.departed)) {
        sintra::finalize();
        return 1;
    }

    std::this_thread::sleep_for(kCleanupGrace);
    const bool cleanup_ok = sintra::barrier("cleanup-ready", "_sintra_all_processes");

    sintra::deactivate_all_slots();
    sintra::finalize();
    return cleanup_ok ? 0 : 1;
}

int run_swing(const Lifecycle_handlers& handlers)
{
    sintra::world() << SwingBarrierSignal{};

    if (!wait_future(handlers.joined)) {
        sintra::finalize();
        return 1;
    }

    if (!wait_future(handlers.departed)) {
        sintra::finalize();
        return 1;
    }

    sintra::deactivate_all_slots();
    sintra::finalize();
    return 0;
}

int run_role(Role role, const std::string& binary_path)
{
    auto handlers = install_common_handlers();

    switch (role) {
    case Role::Coordinator:
        return run_coordinator(handlers);
    case Role::Spawner:
        return run_spawner(handlers, binary_path);
    case Role::Observer:
        return run_observer(handlers);
    case Role::Swing:
        return run_swing(handlers);
    }

    sintra::finalize();
    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string binary_path = argv[0];
    const Role role = determine_role(argc, argv);

    if (role == Role::Coordinator) {
        std::vector<sintra::Process_descriptor> branches;
        branches.emplace_back(binary_path, std::vector<std::string>{"--lifecycle-role=spawner"});
        branches.emplace_back(binary_path, std::vector<std::string>{"--lifecycle-role=observer"});
        sintra::init(argc, argv, branches);
    }
    else {
        sintra::init(argc, argv);
    }

    return run_role(role, binary_path);
}
