#include <sintra/shared_mutex.h>

#include <array>
#include <barrier>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

int main()
{
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t iterations = 64;

    std::array<sintra::shared_mutex, iterations> mutexes;
    std::barrier start_line(static_cast<std::ptrdiff_t>(thread_count));
    std::vector<std::thread> threads;

    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        threads.emplace_back([&] {
            for (auto& mutex : mutexes) {
                start_line.arrive_and_wait();
                {
                    std::shared_lock lock(mutex);
                    start_line.arrive_and_wait();
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    sintra::shared_mutex mutex;
    bool acquired = false;
    {
        std::unique_lock lock(mutex);
        std::thread contender([&] {
            acquired = mutex.try_lock_shared();
            if (acquired) {
                mutex.unlock_shared();
            }
        });
        contender.join();
        if (acquired) {
            return 1;
        }
    }
    {
        std::shared_lock lock(mutex);
        std::thread contender([&] {
            acquired = mutex.try_lock();
            if (acquired) {
                mutex.unlock();
            }
        });
        contender.join();
        if (acquired) {
            return 2;
        }
    }
#ifdef _WIN32
    if (!mutex.try_lock()) {
        return 3;
    }
    mutex.unlock();
    if (!mutex.try_lock_shared()) {
        return 4;
    }
    mutex.unlock_shared();
#endif
}
