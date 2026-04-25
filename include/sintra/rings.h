// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

///\file
///\brief Public facade for Sintra's direct ring helpers.
///
/// Include this header when using Sintra's shared-memory ring primitives
/// directly, without the high-level managed-process IPC layer.
///
/// The high-level publish/subscribe, RPC, barrier, and lifecycle APIs remain
/// available through `sintra/sintra.h`.

#include "detail/ipc/rings.h"
