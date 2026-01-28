#pragma once

#include <stdexcept>
#include <string>
#include <vector>
#include <sstream>

#include "id_types.h"

namespace sintra {

// Exception thrown when sintra::init() fails to initialize the process swarm.
// Provides detailed information about which processes failed and why.
class init_error : public std::runtime_error {
public:
    enum class cause {
        spawn_failed,      // Operating system failed to spawn the process
        barrier_timeout,   // Process spawned but didn't reach initialization barrier
        ipc_setup_failed   // IPC communication setup failed
    };

    struct failed_process {
        std::string binary_name;
        instance_id_type instance_id;
        cause failure_cause;
        int errno_value;           // For spawn_failed: errno from spawn attempt
        std::string error_message; // Human-readable error description

        failed_process(
            std::string bin_name,
            instance_id_type inst_id,
            cause c,
            int err_val = 0,
            std::string err_msg = "")
            : binary_name(std::move(bin_name))
            , instance_id(inst_id)
            , failure_cause(c)
            , errno_value(err_val)
            , error_message(std::move(err_msg))
        {}
    };

private:
    std::vector<failed_process> m_failures;
    std::vector<instance_id_type> m_successful_spawns;
    std::string m_diagnostic;

    static std::string build_message(const std::vector<failed_process>& failures) {
        std::ostringstream oss;
        oss << "sintra::init() failed: " << failures.size()
            << " process(es) could not be initialized";
        return oss.str();
    }

    static std::string build_diagnostic(
        const std::vector<failed_process>& failures,
        const std::vector<instance_id_type>& successful)
    {
        std::ostringstream oss;
        oss << "======================================================================\n";
        oss << "SINTRA INITIALIZATION FAILURE\n";
        oss << "======================================================================\n\n";

        if (!successful.empty()) {
            oss << "Successfully spawned: " << successful.size() << " process(es)\n";
            for (auto id : successful) {
                oss << "  - Instance ID: " << id << "\n";
            }
            oss << "\n";
        }

        oss << "Failed to initialize: " << failures.size() << " process(es)\n\n";

        for (size_t i = 0; i < failures.size(); ++i) {
            const auto& f = failures[i];
            oss << "Process #" << (i + 1) << ":\n";
            oss << "  Binary:      " << f.binary_name << "\n";
            oss << "  Instance ID: " << f.instance_id << "\n";
            oss << "  Cause:       ";

            switch (f.failure_cause) {
                case cause::spawn_failed:
                    oss << "Operating system failed to spawn process\n";
                    if (f.errno_value != 0) {
                        oss << "  Error code:  " << f.errno_value << "\n";
                    }
                    break;
                case cause::barrier_timeout:
                    oss << "Process spawned but did not reach initialization barrier\n";
                    oss << "               (process may have crashed during startup)\n";
                    break;
                case cause::ipc_setup_failed:
                    oss << "IPC communication setup failed\n";
                    break;
            }

            if (!f.error_message.empty()) {
                oss << "  Message:     " << f.error_message << "\n";
            }
            oss << "\n";
        }

        oss << "======================================================================\n";
        oss << "Initialization cannot continue. Please check:\n";
        oss << "  - Binary paths are correct\n";
        oss << "  - File permissions allow execution\n";
        oss << "  - System resources (memory, file descriptors) are available\n";
        oss << "  - Child processes are not crashing during startup\n";
        oss << "======================================================================\n";

        return oss.str();
    }

public:
    init_error(
        std::vector<failed_process> failures,
        std::vector<instance_id_type> successful = {})
        : std::runtime_error(build_message(failures))
        , m_failures(std::move(failures))
        , m_successful_spawns(std::move(successful))
        , m_diagnostic(build_diagnostic(m_failures, m_successful_spawns))
    {}

    // Get list of processes that failed to initialize
    const std::vector<failed_process>& failures() const { return m_failures; }

    // Get list of processes that successfully spawned (even if init overall failed)
    const std::vector<instance_id_type>& successful_spawns() const {
        return m_successful_spawns;
    }

    // Get detailed diagnostic report suitable for logging/stderr
    const std::string& diagnostic_report() const { return m_diagnostic; }
};

} // namespace sintra
