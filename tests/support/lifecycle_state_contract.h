#pragma once

#include "lifecycle_state_probe.h"

#include <cstdio>
#include <string>
#include <vector>

static bool same_lifecycle_identity(const Lifecycle_state_snapshot& expected)
{
    const auto local = capture_lifecycle_state();
    const auto peer = capture_peer_lifecycle_state();
    if (expected.addresses != local.addresses ||
        local.addresses != peer.addresses ||
        expected.signal_handler != local.signal_handler ||
        local.signal_handler != peer.signal_handler)
    {
        std::fprintf(stderr, "Signal/lifeline state differs across translation units\n");
        return false;
    }
    return true;
}

static int run_lifecycle_state_contract(int argc, char* argv[])
{
    const auto initial = capture_lifecycle_state();
    if (!same_lifecycle_identity(initial)) {
        return 1;
    }

    uintptr_t transport = 0;
    for (int cycle = 0; cycle != 2; ++cycle) {
        const int exit_code = 73 + cycle;
        const int timeout_ms = 431 + cycle;
        std::vector<std::string> arguments(argv, argv + argc);
        arguments.push_back("--lifeline_exit_code=" + std::to_string(exit_code));
        arguments.push_back("--lifeline_timeout_ms=" + std::to_string(timeout_ms));
        if (cycle == 0) {
            arguments.push_back("--lifeline_disable");
        }
        std::vector<char*> raw_arguments;
        for (auto& argument : arguments) {
            raw_arguments.push_back(argument.data());
        }
        raw_arguments.push_back(nullptr);
        const int argument_count = static_cast<int>(arguments.size());
        if (cycle == 0) {
            sintra::init(argument_count, raw_arguments.data());
        }
        else {
            initialize_peer_lifecycle(argument_count, raw_arguments.data());
        }

        const auto local = capture_lifecycle_state();
        const auto peer = capture_peer_lifecycle_state();
        bool valid = same_lifecycle_identity(initial) &&
            local.process && local.process == peer.process &&
            local.dispatcher_ready && peer.dispatcher_ready &&
            local.transport == peer.transport &&
            local.exit_code == exit_code && peer.exit_code == exit_code &&
            local.timeout_ms == timeout_ms && peer.timeout_ms == timeout_ms &&
            local.disabled == (cycle == 0) && peer.disabled == (cycle == 0);
#ifdef _WIN32
        valid = valid && local.generation_active && peer.generation_active;
#endif
        if (cycle == 0) {
            transport = local.transport;
        }
        else {
            valid = valid && transport == local.transport;
        }

        const bool finalized = cycle == 0
            ? shutdown_peer_lifecycle()
            : sintra::shutdown();
        const auto stopped = capture_lifecycle_state();
        const auto peer_stopped = capture_peer_lifecycle_state();
        valid = valid && finalized && same_lifecycle_identity(initial) &&
            !stopped.process && !peer_stopped.process &&
            stopped.dispatcher_ready && peer_stopped.dispatcher_ready &&
            stopped.transport == transport && peer_stopped.transport == transport;
#ifdef _WIN32
        valid = valid && !stopped.generation_active && !peer_stopped.generation_active;
#endif
        if (!valid) {
            std::fprintf(stderr, "Cross-TU lifecycle contract failed in cycle %d\n", cycle);
            return 1;
        }
    }
    return 0;
}
