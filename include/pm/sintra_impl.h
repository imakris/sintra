/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef __SINTRA_IMPL_H__
#define __SINTRA_IMPL_H__


namespace sintra {


using std::ostringstream;
using std::string;
using std::vector;


namespace detail {

    inline
    void branch() { }
    template <typename ...Args>
    void branch(const Process_descriptor& first, Args&&... rest)
    {
        branch_vector::s.push_back(first);
        detail::branch(rest...);
    }
}



// Single-process version
inline
void start(int argc, char* argv[], int(*entry)())
{
    mproc::s = new Managed_process;
    mproc::s->init(argc, argv);

    branch_vector::s.clear();
    mproc::s->start(entry);

    delete mproc::s;
}



// Multiple-process version
template <typename ...Args>
void start(int argc, char* argv[], const Process_descriptor& first, Args&&... rest)
{

#ifndef _WIN32
    struct sigaction noaction;
    memset(&noaction, 0, sizeof(noaction));
    noaction.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &noaction, 0);
    setsid();
#endif

    mproc::s = new Managed_process;
    mproc::s->init(argc, argv);

    branch_vector::s.clear();
    detail::branch(first, rest...);
    mproc::s->branch();
    mproc::s->start();

    delete mproc::s;
}



inline
void stop()
{
    mproc::s->stop();
}



inline
void wait_for_stop()
{
    mproc::s->wait_for_stop();
}



// This is a convenience function.
template <typename PROCESS_GROUP_TYPE>
void barrier()
{
    PROCESS_GROUP_TYPE::barrier();
}


template <typename FT>
auto activate_slot(const FT& slot_function, instance_id_type sender_id)
{
    return mproc::s->activate(slot_function, sender_id);
}


inline
void deactivate_slot(Transceiver::handler_provoker_desrcriptor hpd)
{
    return mproc::s->deactivate(hpd);
}



template <instance_id_type LOCALITY>
struct Maildrop
{

    // an array of char turns to an std::string.
    template <size_t N>
    Maildrop& operator << (const char(&value)[N])
    {
        using MT = Message<Enclosure<string>>;
        mproc::s->send<MT, LOCALITY>(string(value));
        return *this;
    }

    // a c string turns to an std::string.
    Maildrop& operator << (const char* value)
    {
        using MT = Message<Enclosure<string>>;
        mproc::s->send<MT, LOCALITY>(string(value));
        return *this;
    }

    // an array of anything other than a char turns to a vector
    template <size_t N, typename T>
    Maildrop& operator << (const T(&v)[N])
    {
        using MT = Message<Enclosure<vector<T>>>;
        mproc::s->send<MT, LOCALITY>(vector<T>(v, v + sizeof v / sizeof *v));
        return *this;
    }

    template <typename T>
    Maildrop& operator << (const T& value)
    {
        using MT = Message<Enclosure<T>>;
        mproc::s->send<MT, LOCALITY>(value);
        return *this;
    }


};


inline Maildrop<any_local>&           local()  { static Maildrop<any_local>           m; return m; }
inline Maildrop<any_remote>&          remote() { static Maildrop<any_remote>          m; return m; }
inline Maildrop<any_local_or_remote>& world()  { static Maildrop<any_local_or_remote> m; return m; }



struct Console
{
    template <typename T>
    Console& operator << (const T& value)
    {
        ostringstream oss;
        oss << value;
        Coordinator::rpc_print(coord_id::s, oss.str());

        return *this;
    }
};



inline
Console& console()
{
    static Console w;
    return w;
}



} // namespace sintra



#endif
