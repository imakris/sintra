//
// Sintra library, example 2
//
// This example introduces Transceivers and RPC and demonstrates
// the following library features:
// - how member functions of Transceiver derivatives can be exported for RPC
// - how to assign a name on the object, which is usable across processes
// - how to make the remote call of the exported method, using the known
//   name of the object
// - RPC failure and interprocess exceptions
//
// In this example, Remotely_accessible is a struct defined to offer an
// 'append' method, which takes a string and an integer and returns another
// string with its arguments concatenated. If the supplied string exceeds
// 10 characters, it throws an exception, which is then transferred to the
// calling process.
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
        if (s.size() > 10) {
            throw std::logic_error("string too long");
        }
        else
        if (v == 2020) {
            // crash
            *((int*)0) = 0;
        }
        return to_string(v) + ": " + s;
    }

    SINTRA_RPC(append)
    //SINTRA_RPC_ONLY(append)
};



void print_city_year(const char* city, int year)
{
    try {
        console() << Remotely_accessible::rpc_append("instance name", city, year) << "\n";
    }
    catch (std::exception& e) {
        console() << "An RPC exception was thrown: " << e.what() << "\n";
    }
}



int process_1()
{
    Remotely_accessible ra;

    ra.assign_name("instance name");

    // ensure that the instance has been named before attempting
    // to access it from another process
    barrier("1st barrier");


    console() << ra.append("Sydney", 2000) << "\n";

    // The RPC mechanism may as well be used with local Transceiver instances.
    // Depending on how the method is exported, it may either resort to a local call,
    // or be forced to go through the interprocess communication rings.
    print_city_year("Athens", 2004);

    // ensure that ra still exists
    barrier("2nd barrier");
    return 0;
}



int process_2()
{
    // ensure that the object has been named before attempting
    // to access it with its name
    barrier("1st barrier");

    string test_string;
    print_city_year("Beijing", 2008);
    print_city_year("Rio de Janeiro", 2016);
    print_city_year("Tokyo", 2020);
    print_city_year("Paris", 2024);

    // ensure that the remotely accessible instance still exists
    barrier("2nd barrier");

    //console() << test_string << "\n";

    return 0;
}


int main(int argc, char* argv[])
{
    init(argc, argv, process_1, process_2);
    finalize();

    if (process_index() == 0) {
        do {
            cout << '\n' << "Press ENTER to continue...";
        } while (cin.get() != '\n');
    }

    return 0;
}
