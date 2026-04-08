#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <exception>
#include <cstdio>
#include <memory>
#include <string>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_prefix = "teardown_targeted_rpc_exception_test: ";
constexpr const char* k_expected_message = "RPC target is no longer available.";

struct Teardown_service : sintra::Derived_transceiver<Teardown_service>
{
    int echo(int value)
    {
        return value + 1;
    }

    SINTRA_RPC_STRICT(echo)
};

struct Shutdown_state_guard
{
    ~Shutdown_state_guard()
    {
        sintra::detail::s_shutdown_state.store(
            sintra::detail::shutdown_protocol_state::idle,
            std::memory_order_release);
    }
};

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    sintra::init(argc, argv);

    bool ok = true;
    {
        auto service = std::make_unique<Teardown_service>();
        const auto target_iid = service->instance_id();
        Shutdown_state_guard state_guard;

        sintra::detail::s_shutdown_state.store(
            sintra::detail::shutdown_protocol_state::finalizing,
            std::memory_order_release);
        service.reset();

        auto handle = Teardown_service::rpc_async_echo(target_iid, 7);
        const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 2s);
        ok &= sintra::test::assert_true(
            wait_status == sintra::Rpc_wait_status::completed,
            k_prefix,
            "late targeted RPC should complete with an exception reply");

        if (wait_status == sintra::Rpc_wait_status::completed) {
            try {
                (void)handle.get();
                ok &= sintra::test::assert_true(
                    false,
                    k_prefix,
                    "late targeted RPC unexpectedly returned a value");
            }
            catch (const std::runtime_error& ex) {
                ok &= sintra::test::assert_true(
                    std::string(ex.what()) == k_expected_message,
                    k_prefix,
                    "late targeted RPC returned the wrong runtime_error message");
            }
            catch (const sintra::rpc_cancelled&) {
                ok &= sintra::test::assert_true(
                    false,
                    k_prefix,
                    "late targeted RPC should not be cancelled");
            }
            catch (const std::exception& ex) {
                std::fprintf(stderr,
                             "%.*slate targeted RPC threw unexpected std::exception: %s\n",
                             static_cast<int>(k_prefix.size()),
                             k_prefix.data(),
                             ex.what());
                ok = false;
            }
            catch (...) {
                sintra::test::print_test_message(
                    k_prefix,
                    "late targeted RPC threw a non-std exception");
                ok = false;
            }
        }
    }

    sintra::detail::finalize();
    return ok ? 0 : 1;
}
