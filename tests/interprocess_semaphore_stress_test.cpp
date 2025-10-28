#include "sintra/detail/interprocess_semaphore.h"
#include "sintra/detail/interprocess_mutex.h"
#include "sintra/detail/ipc_platform_utils.h"
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cassert>

using namespace sintra::detail;
namespace bip = boost::interprocess;

struct SharedState {
    interprocess_semaphore sem;
    interprocess_mutex mutex;
    std::atomic<int> resource_in_use[5];
    std::atomic<int> log_counter;

    SharedState() : sem(5) {
        for (int i = 0; i < 5; ++i) {
            resource_in_use[i].store(-1);
        }
        log_counter.store(0);
    }
};

void log_message(SharedState* state, int pid, const std::string& message) {
    bip::scoped_lock<interprocess_mutex> lock(state->mutex);
    std::ofstream log_file("semaphore_stress_test.log", std::ios_base::app);
    if (log_file.is_open()) {
        log_file << state->log_counter.fetch_add(1) << ": [" << pid << "] " << message << std::endl;
    }
}

[[noreturn]] void fail_test(SharedState* state, int pid, const std::string& error_message) {
    log_message(state, pid, "ERROR: " + error_message);
    std::cerr << "ERROR: [" << pid << "] " << error_message << std::endl;
    exit(1);
}


void child_process(SharedState* state, int process_id) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> sleep_dist(10, 100);

    log_message(state, process_id, "Process started.");

    for (int i = 0; i < 10; ++i) {
        log_message(state, process_id, "Attempting to acquire semaphore.");
        if (!state->sem.timed_wait(std::chrono::steady_clock::now() + std::chrono::seconds(5))) {
            fail_test(state, process_id, "Failed to acquire semaphore within 5 seconds.");
        }

        int resource_id = -1;
        for (int j = 0; j < 5; ++j) {
            int expected = -1;
            if (state->resource_in_use[j].compare_exchange_strong(expected, process_id)) {
                resource_id = j;
                break;
            }
        }

        if (resource_id == -1) {
            state->sem.post();
            fail_test(state, process_id, "No available resource found after acquiring semaphore.");
        }

        log_message(state, process_id, "Acquired semaphore and resource " + std::to_string(resource_id));

        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(gen)));

        int expected = process_id;
        if (!state->resource_in_use[resource_id].compare_exchange_strong(expected, -1)) {
             fail_test(state, process_id, "Resource " + std::to_string(resource_id) + " was not held by this process.");
        }

        log_message(state, process_id, "Releasing resource " + std::to_string(resource_id) + " and posting semaphore.");
        state->sem.post();
    }

    log_message(state, process_id, "Process finished.");
    exit(0);
}

int main() {
    struct shm_remove {
        shm_remove() { bip::shared_memory_object::remove("SintraSemaphoreStressTest"); }
        ~shm_remove() { bip::shared_memory_object::remove("SintraSemaphoreStressTest"); }
    } remover;

    bip::managed_shared_memory segment;
    try {
        segment = bip::managed_shared_memory(bip::create_only, "SintraSemaphoreStressTest", 65536);
    } catch (const bip::interprocess_exception& e) {
        std::cerr << "Failed to create shared memory: " << e.what() << std::endl;
        bip::shared_memory_object::remove("SintraSemaphoreStressTest");
        segment = bip::managed_shared_memory(bip::create_only, "SintraSemaphoreStressTest", 65536);
    }
    SharedState* state = segment.construct<SharedState>("SharedState")();

    std::remove("semaphore_stress_test.log");

    const int num_processes = 10;
    std::vector<pid_t> pids;

    for (int i = 0; i < num_processes; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child
            child_process(state, getpid());
        } else if (pid > 0) {
            // Parent
            pids.push_back(pid);
        } else {
            std::cerr << "Failed to fork process." << std::endl;
            return 1;
        }
    }

    int failures = 0;
    for (pid_t pid : pids) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            std::cerr << "Child process " << pid << " exited with status " << WEXITSTATUS(status) << std::endl;
            failures++;
        } else if (WIFSIGNALED(status)) {
            std::cerr << "Child process " << pid << " terminated by signal " << WTERMSIG(status) << std::endl;
            failures++;
        }
    }

    if (failures > 0) {
        std::cerr << failures << " child processes failed." << std::endl;
        return 1;
    }

    std::cout << "All child processes completed successfully." << std::endl;
    return 0;
}
