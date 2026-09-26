#include "lifecycle_state_probe.h"

Lifecycle_state_snapshot capture_peer_lifecycle_state()
{
    return capture_lifecycle_state();
}

void initialize_peer_lifecycle(int argc, char* argv[])
{
    sintra::init(argc, argv);
}

bool shutdown_peer_lifecycle()
{
    return sintra::shutdown();
}
