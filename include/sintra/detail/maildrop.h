#pragma once

#include "globals.h"
#include "managed_process.h"
#include "message.h"

#include <string>
#include <vector>

namespace sintra {

template <instance_id_type LOCALITY>
struct Maildrop
{
    using mp_type = Managed_process::Transceiver_type;

    template <std::size_t N>
    Maildrop& operator<<(const char (&value)[N])
    {
        using MT = Message<Enclosure<std::string>>;
        s_mproc->send<MT, LOCALITY, mp_type>(std::string(value));
        return *this;
    }

    Maildrop& operator<<(const char* value)
    {
        using MT = Message<Enclosure<std::string>>;
        s_mproc->send<MT, LOCALITY, mp_type>(std::string(value));
        return *this;
    }

    template <std::size_t N, typename T>
    Maildrop& operator<<(const T (&values)[N])
    {
        using MT = Message<Enclosure<std::vector<T>>>;
        s_mproc->send<MT, LOCALITY, mp_type>(std::vector<T>(values, values + N));
        return *this;
    }

    template <typename T>
    Maildrop& operator<<(const T& value)
    {
        using MT = Message<Enclosure<T>>;
        s_mproc->send<MT, LOCALITY, mp_type>(value);
        return *this;
    }
};

inline Maildrop<any_local>& local()
{
    static Maildrop<any_local> instance;
    return instance;
}

inline Maildrop<any_remote>& remote()
{
    static Maildrop<any_remote> instance;
    return instance;
}

inline Maildrop<any_local_or_remote>& world()
{
    static Maildrop<any_local_or_remote> instance;
    return instance;
}

} // namespace sintra
