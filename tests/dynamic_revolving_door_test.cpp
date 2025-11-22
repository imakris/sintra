// Dynamic process churn test for Sintra join/leave support.

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

struct Hello {
    sintra::instance_id_type from;
};

struct Hello_response {
    sintra::instance_id_type target;
};

int worker_main(const std::string& swarm_dir)
{
    if (!sintra::join(swarm_dir, "dynamic_worker")) {
        return 1;
    }

    std::promise<void> got_reply;
    auto reply_future = got_reply.get_future();

    auto handler = [&](const Hello_response& msg) {
        if (msg.target == s_mproc_id) {
            try {
                got_reply.set_value();
            }
            catch (const std::future_error&) {
            }
        }
    };
    sintra::activate_slot(handler);

    sintra::world() << Hello{s_mproc_id};

    const auto status = reply_future.wait_for(std::chrono::seconds(5));
    sintra::finalize();
    return status == std::future_status::ready ? 0 : 1;
}

int main(int argc, char* argv[])
{
    if (argc > 2 && std::string(argv[1]) == "--worker") {
        return worker_main(argv[2]);
    }

    sintra::init(argc, argv);

    std::atomic<int> hellos_seen{0};
    std::atomic<bool> completed{false};
    std::promise<void> all_seen;
    auto all_seen_future = all_seen.get_future();

    auto handler = [&](const Hello& msg) {
        sintra::world() << Hello_response{msg.from};
        if (hellos_seen.fetch_add(1) + 1 >= 50 && !completed.exchange(true)) {
            all_seen.set_value();
        }
    };
    sintra::activate_slot(handler);

    auto* mproc = sintra::runtime_state::instance().managed_process();
    if (!mproc) {
        return 1;
    }
    const std::string swarm_dir = mproc->m_directory;

    bool ok = true;
    for (int i = 0; i < 50; ++i) {
        std::ostringstream cmd;
        cmd << "\"" << argv[0] << "\" --worker \"" << swarm_dir << "\"";
        int rc = std::system(cmd.str().c_str());
        if (rc != 0) {
            ok = false;
            break;
        }
    }

    if (ok) {
        all_seen_future.wait_for(std::chrono::seconds(10));
        ok = all_seen_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    sintra::finalize();
    return ok ? 0 : 1;
}
