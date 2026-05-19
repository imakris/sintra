// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "process/managed_process.h"
#include "messaging/message.h"

#include <string>
#include <vector>

namespace sintra {

/// Maildrop provides a streaming API for broadcasting simple data values.
///
/// Usage:
///   world() << myValue;   // broadcast to all (local + remote)
///   local() << myValue;   // send to local recipients only
///   remote() << myValue;  // send to remote recipients only
///
/// Values are automatically wrapped in Message<Enclosure<T>>. The sender is
/// always the managed process (s_mproc).
///
/// For sending typed protocol messages from a specific transceiver, use the
/// emit_* methods on Derived_transceiver instead (emit_local, emit_remote,
/// emit_global). Those allow specifying explicit message types and preserve
/// the sender's transceiver identity.

template <instance_id_type LOCALITY>
struct Maildrop
{
    using mp_type = Managed_process::Transceiver_type;

    template <typename Payload>
    Maildrop& send_payload(Payload&& payload)
    {
        using MT = Message<Enclosure<std::remove_cvref_t<Payload>>>;
        s_mproc->send<MT, LOCALITY, mp_type>(std::forward<Payload>(payload));
        return *this;
    }

    template <std::size_t N>
    Maildrop& operator<<(const char (&value)[N])
    {
        return send_payload(std::string(value));
    }

    Maildrop& operator<<(const char* value)
    {
        return send_payload(std::string(value));
    }

    template <std::size_t N, typename T>
    Maildrop& operator<<(const T (&values)[N])
    {
        return send_payload(std::vector<T>(values, values + N));
    }

    template <typename T>
    Maildrop& operator<<(const T& value)
    {
        return send_payload(value);
    }
};

template <instance_id_type Locality>
inline Maildrop<Locality>& maildrop()
{
    static Maildrop<Locality> instance;
    return instance;
}

inline Maildrop<any_local>&            local()  { return maildrop<any_local>();            }
inline Maildrop<any_remote>&           remote() { return maildrop<any_remote>();           }
inline Maildrop<any_local_or_remote>&  world()  { return maildrop<any_local_or_remote>();  }

} // namespace sintra
