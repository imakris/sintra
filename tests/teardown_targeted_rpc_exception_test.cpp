#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr const char* k_env_name = "SINTRA_TEST_SHARED_DIR";
constexpr const char* k_test_name = "teardown_targeted_rpc_exception";
constexpr const char* k_target_file = "target_instance_id.txt";
constexpr const char* k_worker_ready_file = "worker_ready.txt";
constexpr const char* k_worker_result_file = "worker_result.txt";
constexpr const char* k_expected_message = "RPC target is no longer available.";

struct Teardown_service;
std::unique_ptr<Teardown_service> g_service;

struct Teardown_service : sintra::Derived_transceiver<Teardown_service>
{
    int echo(int value)
    {
        return value + 1;
    }

    SINTRA_RPC_STRICT(echo)
};

void write_text_file(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + path.string());
    }
    out << text;
}

sintra::instance_id_type read_instance_id(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    unsigned long long value = 0;
    in >> value;
    return static_cast<sintra::instance_id_type>(value);
}

int worker_process()
{
    sintra::test::Shared_directory shared(k_env_name, k_test_name);
    const auto dir = shared.path();
    const auto target_path = dir / k_target_file;
    const auto ready_path = dir / k_worker_ready_file;
    const auto result_path = dir / k_worker_result_file;

    if (!sintra::test::wait_for_path(target_path, 5s)) {
        write_text_file(result_path, "fail: target iid missing\n");
        return 1;
    }

    const auto target_iid = read_instance_id(target_path);
    std::mutex mutex;
    std::condition_variable cv;
    bool target_unpublished = false;

    sintra::activate_slot(
        [&](const sintra::Coordinator::instance_unpublished& msg) {
            if (msg.instance_id != target_iid) {
                return;
            }

            std::lock_guard<std::mutex> lock(mutex);
            target_unpublished = true;
            cv.notify_one();
        },
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    write_text_file(ready_path, "ready\n");

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 5s, [&]() { return target_unpublished; })) {
            write_text_file(result_path, "fail: target was not unpublished\n");
            return 1;
        }
    }

    auto handle = Teardown_service::rpc_async_echo(target_iid, 7);
    if (handle.wait_until(std::chrono::steady_clock::now() + 5s) !=
        sintra::Rpc_wait_status::completed)
    {
        write_text_file(result_path, "fail: rpc did not complete\n");
        return 1;
    }

    try {
        (void)handle.get();
        write_text_file(result_path, "fail: rpc unexpectedly returned\n");
        return 1;
    }
    catch (const std::runtime_error& ex) {
        if (std::string(ex.what()) == k_expected_message) {
            write_text_file(result_path, "ok\n");
            return 0;
        }

        write_text_file(result_path, std::string("fail: wrong message: ") + ex.what() + "\n");
        return 1;
    }
    catch (const sintra::rpc_cancelled&) {
        write_text_file(result_path, "fail: rpc cancelled\n");
        return 1;
    }
    catch (const std::exception& ex) {
        write_text_file(result_path, std::string("fail: unexpected std::exception: ") + ex.what() + "\n");
        return 1;
    }
    catch (...) {
        write_text_file(result_path, "fail: unexpected non-std exception\n");
        return 1;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    return sintra::test::run_multi_process_test_raw(
        argc,
        argv,
        k_env_name,
        k_test_name,
        {worker_process},
        [](const std::filesystem::path& dir) {
            std::error_code ec;
            std::filesystem::remove(dir / k_target_file, ec);
            std::filesystem::remove(dir / k_worker_ready_file, ec);
            std::filesystem::remove(dir / k_worker_result_file, ec);
        },
        [](const std::filesystem::path& dir) {
            g_service = std::make_unique<Teardown_service>();

            write_text_file(
                dir / k_target_file,
                std::to_string(static_cast<unsigned long long>(g_service->instance_id())) + "\n");

            if (!sintra::test::wait_for_path(dir / k_worker_ready_file, 5s)) {
                std::fprintf(stderr, "teardown_targeted_rpc_exception_test: worker did not become ready\n");
                return 1;
            }

            std::this_thread::sleep_for(50ms);
            return 0;
        },
        [](const std::filesystem::path& dir) {
            g_service.reset();

            std::ifstream in(dir / k_worker_result_file, std::ios::binary);
            std::string result;
            std::getline(in, result);

            if (result != "ok") {
                if (result.empty()) {
                    std::fprintf(stderr, "teardown_targeted_rpc_exception_test: missing worker result\n");
                }
                else {
                    std::fprintf(stderr,
                                 "teardown_targeted_rpc_exception_test: worker reported '%s'\n",
                                 result.c_str());
                }
                return 1;
            }

            return 0;
        });
}
