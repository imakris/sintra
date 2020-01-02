//
// Sintra library, example #3
//
// This example introduces Transceivers and RPC and demonstrates the following library features:
// - how member functions of Transceiver derivatives can be exported for RPC
// - how to assign a name on the object, which is usable across processes
// - how to make the remote call of the exported method, using the known name of the object
//
// In this example, Remotely_accessible is a struct defined to offer an 'append' method,
// which takes a string and an integer and returns another string with its arguments concatenated.
// This method is exported for RPC.
// An instance of Remotely_accessible is local to process_1, and used locally.
// This same instance is used by process_2 remotely.

#include <sintra/sintra.h>
#include <iostream>


using namespace std;
using namespace sintra;


struct Remotely_accessible: Derived_transceiver<Remotely_accessible>
{
    string append(const string& s, int v)
    {
        return s + to_string(v);
    }

    SINTRA_RPC(append)
};



int process_1()
{
    Remotely_accessible ra;

    ra.assign_name("some name");

    // ensure that the instance has been named before trying to access it from another process
    barrier();

    string test_string = ra.append("sydney_", 2000);

    console() << test_string << "\n";

    // ensure that ra still exists
    barrier();
    return 0;
}



int process_2()
{
    // ensure that the object has been named before trying to access it with its name
    barrier();

    string test_string = Remotely_accessible::rpc_append("some name", "beijing_", 2008);
    
    // ensure that the remotely accessible instance still exists
    barrier();

    console() << test_string << "\n";

    return 0;
}




int main(int argc, char* argv[])
{
    start(
        argc, argv,
        Process_descriptor(process_1)
    ,   Process_descriptor(process_2)
    );

    if (process_index() == 0) {
        do {
            cout << '\n' << "Press ENTER to continue...";
        } while (cin.get() != '\n');
    }

    return 0;
}
