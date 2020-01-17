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

#ifndef SINTRA_IMPL_H
#define SINTRA_IMPL_H


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
        s_branch_vector.push_back(first);
        detail::branch(rest...);
    }
}



// Single-process version
inline
void start(int argc, char* argv[], int(*entry)())
{
    s_mproc = new Managed_process;
    s_mproc->init(argc, argv);

    s_branch_vector.clear();
    s_mproc->start(entry);

    delete s_mproc;
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

    s_mproc = new Managed_process;
    s_mproc->init(argc, argv);

    s_branch_vector.clear();
    detail::branch(first, rest...);
    s_mproc->branch();
    s_mproc->start();

    delete s_mproc;
}



inline
void stop()
{
    s_mproc->stop();
}


inline
bool running()
{
    return s_mproc->m_state == Managed_process::RUNNING;
}



inline
void wait_for_stop()
{
    s_mproc->wait_for_stop();
}


inline
int process_index()
{
    return s_branch_index;
}


// This is a convenience function.
template <typename PROCESS_GROUP_T>
void barrier()
{
    PROCESS_GROUP_T::barrier();
}


template <typename FT, typename SENDER_T>
auto activate_slot(const FT& slot_function, Typed_instance_id<SENDER_T> sender_id)
{
    return s_mproc->activate(slot_function, sender_id);
}



template <instance_id_type LOCALITY>
struct Maildrop
{
    using mp_type = Managed_process::Transceiver_type;

    // an array of char turns to an std::string.
    template <size_t N>
    Maildrop& operator << (const char(&value)[N])
    {
        using MT = Message<Enclosure<string>>;
        s_mproc->send<MT, LOCALITY, mp_type>(string(value));
        return *this;
    }

    // a c string turns to an std::string.
    Maildrop& operator << (const char* value)
    {
        using MT = Message<Enclosure<string>>;
        s_mproc->send<MT, LOCALITY, mp_type>(string(value));
        return *this;
    }

    // an array of anything other than a char turns to a vector
    template <size_t N, typename T>
    Maildrop& operator << (const T(&v)[N])
    {
        using MT = Message<Enclosure<vector<T>>>;
        s_mproc->send<MT, LOCALITY, mp_type>(vector<T>(v, v + sizeof v / sizeof *v));
        return *this;
    }

    template <typename T>
    Maildrop& operator << (const T& value)
    {
        using MT = Message<Enclosure<T>>;
        s_mproc->send<MT, LOCALITY, mp_type>(value);
        return *this;
    }
};


inline Maildrop<any_local>&           local()  { static Maildrop<any_local>           m; return m; }
inline Maildrop<any_remote>&          remote() { static Maildrop<any_remote>          m; return m; }
inline Maildrop<any_local_or_remote>& world()  { static Maildrop<any_local_or_remote> m; return m; }



struct Console
{
    Console(){}
    Console(const Console&)             = delete;
    Console& operator=(const Console&)  = delete;

    ~Console()
    {
        Coordinator::rpc_print(s_coord_id, m_oss.str());
    }

    template <typename T>
    Console& operator << (const T& value)
    {
        m_oss << value;
        return *this;
    }

private:
    ostringstream m_oss;

    // prevent heap instances
    void* operator new(size_t);
    void* operator new(size_t, void*);
    void* operator new[](size_t);
    void* operator new[](size_t, void*);
};


using console = Console;


} // namespace sintra



#endif
