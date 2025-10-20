// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

namespace sintra {


using std::string;


template<typename>
void Resolvable_instance_id::from_string(const string& str)
{
    value = Coordinator::rpc_resolve_instance(s_coord_id, str);
}


} // namespace sintra


