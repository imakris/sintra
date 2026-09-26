#include "fixed_rpc_registration_contract.h"

namespace sintra::test::fixed_rpc_registration {

std::unique_ptr<Stable_service> make_peer_service()
{
    return std::make_unique<Stable_service>();
}

} // namespace sintra::test::fixed_rpc_registration
