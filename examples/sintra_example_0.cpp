//
// Sintra library, example #1
//
// This program demonstrates simple point to point process communication, using one executable.
// A multi-process single executable program requires a call to init_and_branch() in its main thread
// which initializes the sintra library, spawns the process swarm and starts the multi-process
// execution.
//
// In this example, there are 2 user processes, of which the first is acting as a sender, and
// the other two as receivers.
// Each receiver activates a slot function that handles arguments of a different type (int, string).
// The sender process sends messages of both types, which are handled by each process accordingly.
// A barrier is used to ensure that the slots are activated before the messages are sent.

#include <iostream>
#include <sintra/sintra.h>

using namespace std;
using namespace sintra;

int process_1()
{
	// This barrier ensures that the slots receiving the messages have been activated.
	barrier();

	// send some messages
	world() << "good morning";
	world() << 1;
	world() << "good afternoon" << "good evening" << "good night";
	world() << 2 << 3 << 4;

	return 0;
}


int process_2()
{
	auto string_slot = [] (const string& str) {
		cout << "Received string \"" << str << "\"\n";

		static int num_messages = 0;
		if (++num_messages == 4) {
			stop();
		}
	};

	activate_slot(string_slot);
	barrier();

	wait_for_stop();

	return 0;
}


int process_3()
{
	auto int_slot = [&] (int number) {
		cout << "Received number " << number << "\n";

		static int num_messages = 0;
		if (++num_messages == 2) { //it will not receive them all, just 2
			stop();
		}
	};

	activate_slot(int_slot);
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
