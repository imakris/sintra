#pragma once

#include <sintra/sintra.h>

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace sintra::test::fixed_rpc_registration {

constexpr type_id_type k_fixed_rpc_id = make_user_type_id(0x534d1101);

struct Stable_service : Derived_transceiver<Stable_service>
{
    int ping(int value) { return value + 1; }

    SINTRA_RPC_IMPL(ping, &Transceiver_type::ping, k_fixed_rpc_id, false, false)
};

struct Conflicting_service : Derived_transceiver<Conflicting_service>
{
    explicit Conflicting_service(instance_id_type id)
        : Derived_transceiver<Conflicting_service>("", id)
    {}

    int ping(int value) { return value + 2; }

    SINTRA_RPC_IMPL(ping, &Transceiver_type::ping, k_fixed_rpc_id, false, false)
};

struct Dynamic_service : Derived_transceiver<Dynamic_service>
{
    int ping(int value) { return value + 3; }

    SINTRA_RPC_STRICT(ping)
};

std::unique_ptr<Stable_service> make_peer_service();

inline void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void run_cycle()
{
    Stable_service first;
    auto second = make_peer_service();
    Dynamic_service dynamic;
    require(Stable_service::rpc_ping(first.instance_id(), 4) == 5,
        "first fixed export did not complete its normal RPC");
    require(Stable_service::rpc_ping(second->instance_id(), 8) == 9,
        "same fixed export from another translation unit was rejected");
    require(Dynamic_service::rpc_ping(dynamic.instance_id(), 12) == 15,
        "dynamic RPC registration was not usable");

    void (*original_handler)(Message_prefix&) = nullptr;
    {
        auto handlers = Transceiver::get_rpc_handler_map().scoped();
        original_handler = handlers.get().at(k_fixed_rpc_id);
    }

    // Exercise registration rejection only. No request is sent to the
    // conflicting export, including on a regression that accepts it.
    const auto rejected_instance = make_instance_id();
    bool rejected = false;
    try {
        Conflicting_service conflicting(rejected_instance);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "different fixed RPC export was accepted");
    {
        auto handlers = Transceiver::get_rpc_handler_map().scoped();
        require(handlers.get().at(k_fixed_rpc_id) == original_handler,
            "rejected export replaced the existing handler");
    }
    {
        auto instances = Transceiver::get_instance_to_object_map<
            Conflicting_service::ping_mftc>().scoped();
        require(instances.get().find(rejected_instance) == instances.get().end(),
            "rejected export left a failed instance registered");
    }
    {
        auto instances = s_mproc->m_local_pointer_of_instance_id.scoped();
        require(instances.get().find(rejected_instance) == instances.get().end(),
            "failed construction left its transceiver pointer registered");
    }
}

inline int run_contract(int argc, char* argv[])
{
    try {
        for (int cycle = 0; cycle != 2; ++cycle) {
            init(argc, argv);
            run_cycle();
            require(sintra::detail::finalize_impl(), "runtime teardown did not complete");
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "fixed_rpc_registration_test: %s\n", error.what());
        sintra::detail::finalize();
        return 1;
    }
}

} // namespace sintra::test::fixed_rpc_registration
