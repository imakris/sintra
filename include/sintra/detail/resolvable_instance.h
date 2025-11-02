// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "messaging/message.h"


namespace sintra {


using std::string;


struct Resolvable_instance_id: Sintra_message_element
{
    Resolvable_instance_id(instance_id_type v): value(v) {}
    const Resolvable_instance_id& operator= (instance_id_type v)
                                                    { value = v; return *this;          }
    Resolvable_instance_id(const string& str)       { from_string(str);                 }
    Resolvable_instance_id(const char* str)         { from_string(str);                 }
    const Resolvable_instance_id& operator= (const string& str)
                                                    { from_string(str); return *this;   }
    operator instance_id_type() const               { return value;                     }

private:
    instance_id_type value = invalid_instance_id;

    template<typename = void>
    void from_string(const string& str);
};


} // sintra


