// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../../ipc/file_mapping.h"

namespace sintra::detail::ipc {

using sintra::ipc::copy_on_write;
using sintra::ipc::file_mapping;
using sintra::ipc::map_mode_t;
using sintra::ipc::map_options_t;
using sintra::ipc::mapped_region;
using sintra::ipc::read_only;
using sintra::ipc::read_write;

} // namespace sintra::detail::ipc
