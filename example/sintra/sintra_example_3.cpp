//
// Sintra library, example #4
//
// This example demonstrates message dispatch, within the same process.
// It is very similar to example #1.
//
// In this example, there is only one process, with 3 registered slots. Two of them are just
// simply replying with a message of the opposite type of what they are handling, and the thrd is
// counting the cycles.
// 
// Removing the console messages in the ping-pong processes should normally have a substantial
// effect in performance (in this example, they are commented out).
//

#include <iostream>
#include <sintra/sintra.h>
#include <omp.h>


using namespace std;
using namespace sintra;



struct Ping {};
struct Pong {};


int entry_function()
{
	auto ping_slot = [] (Ping) {
		//console() << "received ping, sending pong \n";
		world() << Pong();
	};

	auto pong_slot = [] (Pong) {
		//console() << "received pong, sending ping \n";
		world() << Ping();
	};

	double ts = omp_get_wtime();
	double next_ts = ts + 1.;
	uint64_t counter = 0;

	auto benchmark_slot = [&] (Ping) {
		double ts = omp_get_wtime();
		if (ts > next_ts) {
			next_ts = ts + 1.;
			console() << counter << " ping-pongs / second\n";
			counter = 0;
		}
		counter++;
	};

	activate_slot(ping_slot);
	activate_slot(pong_slot);
	activate_slot(benchmark_slot);

	// the spark
	world() << Ping();

	wait_for_stop();
	return 0;
}



int main(int argc, char* argv[])
{
	start(argc, argv, entry_function);

	return 0;
}
