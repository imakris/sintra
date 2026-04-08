#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_prefix = "teardown_targeted_rpc_exception_test: ";
constexpr const char* k_expected_message = "RPC target is no longer available.";
constexpr auto k_ready_timeout = std::chrono::seconds(10);
constexpr auto k_poll_interval = std::chrono::milliseconds(50);

struct Teardown_service : sintra::Derived_transceiver<Teardown_service>
{
    int echo(int value)
    {
        return value + 1;
    }

    SINTRA_RPC_STRICT(echo)
};

struct Teardown_admission_guard
{
    explicit Teardown_admission_guard(const char* api_name)
    {
        sintra::detail::close_teardown_admission_and_claim_state(
            sintra::detail::shutdown_protocol_state::finalizing,
            api_name);
    }

    ~Teardown_admission_guard()
    {
        sintra::detail::reset_lifecycle_teardown_to_idle();
    }
};

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string value;
    std::getline(in, value);
    return value;
}

std::string wait_for_nonempty_text(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto value = read_text(path);
        if (!value.empty()) {
            return value;
        }
        std::this_thread::sleep_for(k_poll_interval);
    }
    return read_text(path);
}

template <typename Handle>
bool expect_runtime_error_reply(
    Handle& handle,
    std::string_view context)
{
    bool ok = true;
    const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 2s);
    ok &= sintra::test::assert_true(
        wait_status == sintra::Rpc_wait_status::completed,
        k_prefix,
        std::string(context) + " should complete with an exception reply");

    if (wait_status != sintra::Rpc_wait_status::completed) {
        return false;
    }

    try {
        (void)handle.get();
        ok &= sintra::test::assert_true(
            false,
            k_prefix,
            std::string(context) + " unexpectedly returned a value");
    }
    catch (const std::runtime_error& ex) {
        ok &= sintra::test::assert_true(
            std::string(ex.what()) == k_expected_message,
            k_prefix,
            std::string(context) + " returned the wrong runtime_error message");
    }
    catch (const sintra::rpc_cancelled&) {
        ok &= sintra::test::assert_true(
            false,
            k_prefix,
            std::string(context) + " should not be cancelled");
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "%.*s%.*s threw unexpected std::exception: %s\n",
                     static_cast<int>(k_prefix.size()),
                     k_prefix.data(),
                     static_cast<int>(context.size()),
                     context.data(),
                     ex.what());
        ok = false;
    }
    catch (...) {
        sintra::test::print_test_message(
            k_prefix,
            std::string(context) + " threw a non-std exception");
        ok = false;
    }

    return ok;
}

bool test_local_targeted_rpc_exception()
{
    auto service = std::make_unique<Teardown_service>();
    const auto target_iid = service->instance_id();

    {
        Teardown_admission_guard guard(
            "teardown_targeted_rpc_exception_test.local_targeted_rpc");
        service.reset();

        auto handle = Teardown_service::rpc_async_echo(target_iid, 7);
        return expect_runtime_error_reply(handle, "late local targeted RPC");
    }
}

std::filesystem::path target_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "target_iid.txt";
}

std::filesystem::path worker_ready_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "worker_ready.txt";
}

std::filesystem::path target_removed_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "target_removed.txt";
}

std::filesystem::path worker_result_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "worker_result.txt";
}

int remote_client_process()
{
    sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR",
        "teardown_targeted_rpc_exception");
    const auto shared_dir = shared.path();

    const auto target_text = wait_for_nonempty_text(target_path(shared_dir), k_ready_timeout);
    if (target_text.empty()) {
        write_text(worker_result_path(shared_dir), "fail: target iid missing");
        return 1;
    }

    const auto target_iid = static_cast<sintra::instance_id_type>(std::stoull(target_text));
    write_text(worker_ready_path(shared_dir), "ready");

    if (!sintra::test::wait_for_file(target_removed_path(shared_dir), k_ready_timeout, k_poll_interval)) {
        write_text(worker_result_path(shared_dir), "fail: target removal not observed");
        return 1;
    }

    auto handle = Teardown_service::rpc_async_echo(target_iid, 7);
    if (!expect_runtime_error_reply(handle, "late remote targeted RPC")) {
        write_text(worker_result_path(shared_dir), "fail: late targeted RPC did not return expected runtime_error");
        return 1;
    }

    write_text(worker_result_path(shared_dir), "ok");
    return 0;
}

int coordinator_action(const std::filesystem::path& shared_dir)
{
    if (!test_local_targeted_rpc_exception()) {
        return 1;
    }

    auto service = std::make_unique<Teardown_service>();
    const auto target_iid = service->instance_id();
    write_text(
        target_path(shared_dir),
        std::to_string(static_cast<unsigned long long>(target_iid)));

    if (wait_for_nonempty_text(worker_ready_path(shared_dir), k_ready_timeout).empty()) {
        std::fprintf(stderr, "%.*sworker did not become ready\n",
                     static_cast<int>(k_prefix.size()),
                     k_prefix.data());
        return 1;
    }

    {
        Teardown_admission_guard guard(
            "teardown_targeted_rpc_exception_test.remote_targeted_rpc");
        service.reset();
        write_text(target_removed_path(shared_dir), "removed");

        const auto worker_result = wait_for_nonempty_text(worker_result_path(shared_dir), k_ready_timeout);
        if (worker_result.empty()) {
            std::fprintf(stderr, "%.*sworker result missing\n",
                         static_cast<int>(k_prefix.size()),
                         k_prefix.data());
            return 1;
        }
        if (worker_result != "ok") {
            std::fprintf(stderr, "%.*s%s\n",
                         static_cast<int>(k_prefix.size()),
                         k_prefix.data(),
                         worker_result.c_str());
            return 1;
        }
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    return sintra::test::run_multi_process_test_raw(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "teardown_targeted_rpc_exception",
        {remote_client_process},
        [](const std::filesystem::path&) {},
        [](const std::filesystem::path& shared_dir) { return coordinator_action(shared_dir); },
        [](const std::filesystem::path&) { return 0; });
}
