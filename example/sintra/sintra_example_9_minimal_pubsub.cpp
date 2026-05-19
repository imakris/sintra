//
// Sintra library, example 9
//
// This is the smallest complete multi-process publish/subscribe program:
// define a payload, start two branches, activate the receiver slot before
// sending, then use a processing fence before coordinated shutdown.

#include <sintra/sintra.h>



struct Market_tick
{
    int    instrument_id;
    double last_price;
};



int sender_branch()
{
    sintra::barrier("market-tick-ready");

    sintra::world() << Market_tick{17, 101.25};

    sintra::barrier<sintra::processing_fence_t>("market-tick-processed");

    return 0;
}


int receiver_branch()
{
    sintra::activate_slot([](const Market_tick& tick) {
        sintra::console()
            << "instrument " << tick.instrument_id
            << " last price " << tick.last_price << '\n';
    });

    sintra::barrier("market-tick-ready");
    sintra::barrier<sintra::processing_fence_t>("market-tick-processed");

    return 0;
}



int main(int argc, char* argv[])
{
    sintra::init(argc, argv, sender_branch, receiver_branch);
    sintra::shutdown();

    return 0;
}
