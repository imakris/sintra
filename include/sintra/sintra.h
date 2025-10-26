// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

///\file
///\brief Public facade for the Sintra library.
///
///  Sintra is a header-only message-passing library built around transceivers
///  and a coordinator that supervises managed processes.  This header is the
///  single entry point that applications should include.  It gathers the pieces
///  that make up the façade API and documents the major components so that
///  nothing is "secret" from consumers of the library.
///
///  The library is composed of four broad areas:
///  - *Core coordination* (coordinator, managed processes, global state helper)
///  - *Messaging primitives* (messages, transceivers, and the request/reply
///    transport)
///  - *Process management* (spawning, branching, recovery)
///  - *Utilities* (configuration, exception conversions, ID management)
///
///  The includes below are intentionally explicit: each block exposes one of
///  the areas above and is followed by a short description so that readers of
///  this façade understand which features they are pulling in.


#include <string>

// -- Platform and toolchain adjustments ------------------------------------
// Sintra ships as headers and can be built with MSVC or GCC/Clang.  The block
// below matches the behaviour of the original project while documenting the
// reasoning behind each macro.

#ifndef BOOST_ALL_NO_LIB
#define BOOST_ALL_NO_LIB
#define SINTRA_UNDEF_BOOST_ALL_NO_LIB
#endif


#ifdef _MSC_VER  // if compiling with Visual Studio

#pragma warning( push )
#pragma warning( disable : 4996 )

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef BOOST_USE_WINDOWS_H
#define BOOST_USE_WINDOWS_H
#endif

#endif // _MSC_VER


#ifndef __cpp_inline_variables
#error Inline variables are not supported. Sintra requires this and other C++17 features.
#endif

#if defined(__clang__)
#   if !__has_feature(cxx_rtti)
#       error "Sintra requires RTTI (Run-Time Type Information). Enable it for your build (e.g. remove -fno-rtti)."
#   endif
#elif defined(__GNUC__)
#   if !defined(__GXX_RTTI)
#       error "Sintra requires RTTI (Run-Time Type Information). Rebuild with -frtti."
#   endif
#elif defined(_MSC_VER)
#   if !defined(_CPPRTTI)
#       error "Sintra requires RTTI (Run-Time Type Information). Rebuild with /GR."
#   endif
#elif !defined(__cpp_rtti)
#   error "Sintra requires RTTI (Run-Time Type Information)."
#endif


// -- Core coordination ------------------------------------------------------
// The coordinator orchestrates the managed processes, keeps track of
// published transceivers, and resolves names.  The runtime_state helper lives
// here as well: it exposes the global singleton that binds together the active
// coordinator and the local managed process instance.
#include "detail/deterministic_delay.h"
#include "detail/globals.h"
#include "detail/coordinator.h"
#include "detail/coordinator_impl.h"
#include "detail/managed_process.h"
#include "detail/managed_process_impl.h"

// -- Messaging --------------------------------------------------------------
// Messages are the communication primitive in Sintra.  They travel over
// transceivers, and the process message reader consumes them on behalf of a
// managed process.  These includes wire up the request/reply rings, message
// envelopes, and the transceiver façade that user code interacts with.
#include "detail/message_impl.h"
#include "detail/process_message_reader_impl.h"
#include "detail/transceiver.h"
#include "detail/transceiver_impl.h"

// -- Resolution helpers -----------------------------------------------------
// Runtime type and instance resolution (e.g. waiting for a named transceiver)
// are implemented as templates that live in the `resolvable_instance` family.
// Keeping the include explicit here documents that the top-level API exposes
// those facilities as part of the public surface.
#include "detail/resolvable_instance_impl.h"

// NOTE: Sintra installs handlers for SIGABRT, SIGFPE, SIGILL, SIGINT, SIGSEGV,
// and SIGTERM. Existing handlers are chained where possible; applications that
// need bespoke signal handling should install their handlers after including
// this header.

namespace sintra {



// ---------------------------------------------------------------------------
// Barrier mode tags
// ---------------------------------------------------------------------------
// The tag types below select the strength of barrier synchronisation:
//   - rendezvous_t: wait until every participant has reached the barrier.
//   - delivery_fence_t (default): additionally ensure all pre-barrier messages
//     have been fetched by local request readers.
//   - processing_fence_t: the strongest guarantee – all pre-barrier message
//     handlers have run to completion everywhere.
struct rendezvous_t {};
struct delivery_fence_t {};
struct processing_fence_t {};

///\brief Synchronise processes participating in a named barrier.
///
/// Blocks the calling thread until at least one thread of every process in
/// `group_name` has reached the barrier.  The template parameter selects the
/// strength of the synchronisation fence (see the tag descriptions above).
///
/// When multiple threads in a single process enter the same barrier they will
/// match with the corresponding threads of the other participants in an
/// unspecified order.  Barrier is an inter-process synchronisation mechanism –
/// combine it with traditional threading primitives if you also need
/// thread-level coordination within a process.
template<typename BarrierMode = delivery_fence_t>
bool barrier(const std::string& barrier_name, const std::string& group_name = "_sintra_external_processes");


template <typename FT, typename SENDER_T = void>
///\brief Activate a slot for handling messages from an optional sender.
///
/// The `slot_function` receives deserialised messages matching the type of the
/// transceiver it is attached to.  Passing an explicit `sender_id` restricts the
/// slot to messages from a specific originator; by default slots listen to any
/// sender (local or remote).
auto activate_slot(
    const FT& slot_function,
    Typed_instance_id<SENDER_T> sender_id = Typed_instance_id<void>(any_local_or_remote) );


///\brief Deactivate all slots owned by the current managed process.
void deactivate_all_slots();


///\brief Enable automatic recovery for the current managed process.
///
/// When recovery is enabled the coordinator will respawn the process if it
/// exits unexpectedly.  Applications that rely on clean shutdown can still call
/// `sintra::finalize()` (included below via the runtime utilities) when they
/// are done.
void enable_recovery();

} // namespace sintra


// -- Runtime utilities ------------------------------------------------------
// High-level synchronisation, logging, message composition helpers, and the
// process runtime live here.  Historically these were gathered in
// `detail/sintra_impl.h`; we now include the specific headers directly to keep
// dependency relationships obvious.
#include "detail/barrier.h"
#include "detail/console.h"
#include "detail/maildrop.h"
#include "detail/runtime.h"


#ifdef _MSC_VER
#pragma warning( pop )
#endif


#ifdef SINTRA_UNDEF_BOOST_ALL_NO_LIB
#undef BOOST_ALL_NO_LIB
#endif
