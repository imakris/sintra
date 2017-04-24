//
// Sintra library, example #2
//
// This example demonstrates the usage of the interprocess console.
//
// In this example, there are 2 user processes, that send ping-pong messages to each other.
// Whenever a ping/pong message is received, an message is sent to the interprocess console.
// A third process is observing the other two and reports the ping-pong rate.
// 
// Removing the console messages in the ping-pong processes should normally have a substantial
// effect in performance (they are currently commented out).
//

#include <iostream>
#include <sintra/sintra.h>
#include <omp.h>


using namespace std;
using namespace sintra;



struct Ping {};
struct Pong {};


int process_1()
{
	auto ping_slot = [] (Ping) {
		//console() << "received ping, sending pong \n";
		world() << Pong();
	};

	activate_slot(ping_slot);
	barrier();

	wait_for_stop();
	return 0;
}


int process_2()
{
	auto pong_slot = [] (Pong) {
		//console() << "received pong, sending ping \n";
		world() << Ping();
	};

	activate_slot(pong_slot);
	barrier();

	// the spark
	world() << Ping();

	wait_for_stop();
	return 0;
}


int process_3()
{
	double ts = omp_get_wtime();
	double next_ts = ts + 1.;
	uint64_t counter = 0;

	auto ping_slot = [&] (Ping) {
		double ts = omp_get_wtime();
		if (ts > next_ts) {
			next_ts = ts + 1.;
			console() << counter << " ping-pongs / second\n";
			counter = 0;
		}
		counter++;
	};

	auto slot_id = activate_slot(ping_slot);
	barrier();

	wait_for_stop();
	return 0;
}



int main(int argc, char* argv[])
{
	start(argc, argv,
		Process_descriptor(process_1)
	,	Process_descriptor(process_2)
	,	Process_descriptor(process_3)
	);

	return 0;
}
