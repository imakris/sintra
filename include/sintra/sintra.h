// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

///\file
///\brief Public facade for the Sintra library.
///
///  Sintra is a header-only message-passing library built around transceivers
///  and a coordinator that supervises managed processes.  This header is the
///  single entry point that applications should include.  It gathers the pieces
///  that make up the faÃ§ade API and documents the major components so that
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
///  this faÃ§ade understand which features they are pulling in.


#include <functional>
#include <string>

// -- Platform and toolchain adjustments ------------------------------------
// Sintra ships as headers and can be built with MSVC or GCC/Clang.  The block
// below matches the behaviour of the original project while documenting the
// reasoning behind each macro.



#ifdef _MSC_VER  // if compiling with Visual Studio
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif // _MSC_VER


#ifndef __cpp_inline_variables
#error Inline variables are not supported. Sintra requires this and other C++17 features.
#endif

// RTTI is not required; Sintra uses compile-time type names for type ids.


// -- Core coordination ------------------------------------------------------
// The coordinator orchestrates the managed processes, keeps track of
// published transceivers, and resolves names.  The runtime_state helper lives
// here as well: it exposes the global singleton that binds together the active
// coordinator and the local managed process instance.
#include "detail/globals.h"
#include "detail/process/coordinator.h"
#include "detail/process/coordinator_impl.h"
#include "detail/process/managed_process.h"
#include "detail/process/managed_process_impl.h"

// -- Messaging --------------------------------------------------------------
// Messages are the communication primitive in Sintra.  They travel over
// transceivers, and the process message reader consumes them on behalf of a
// managed process.  These includes wire up the request/reply rings, message
// envelopes, and the transceiver faÃ§ade that user code interacts with.
//
// RPC is part of this surface as well:
// - `rpc_<method>(...)` remains the blocking call/return API
// - `rpc_async_<method>(...)` returns an `Rpc_handle<T>` for async waiting,
//   deadline-bounded waits, abandonment, and later result retrieval
//
// Typical usage:
// \code
// struct Calculator : sintra::Derived_transceiver<Calculator>
// {
//     int add(int a, int b) { return a + b; }
//     int slow_add(int a, int b) { return a + b; }
//
//     SINTRA_RPC(add)
//     SINTRA_RPC_STRICT(slow_add)
// };
//
// auto sum = Calculator::rpc_add(target, 10, 15); // blocking
//
// auto handle = Calculator::rpc_async_slow_add(target, 10, 15); // async
// if (handle.wait_until(deadline) == sintra::Rpc_wait_status::completed) {
//     auto value = handle.get();
// } else {
//     handle.abandon();
// }
// \endcode
//
// Export choice matters for local targets:
// - `SINTRA_RPC` may take a direct same-process shortcut for blocking RPC
// - `SINTRA_RPC_STRICT` always uses the transported RPC path and is therefore
//   the export to use when the async-handle surface must also work locally
#include "detail/messaging/message_impl.h"
#include "detail/messaging/process_message_reader_impl.h"
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
//     have been fetched by local request readers in this process. It does not
//     perform a second rendezvous to prove that peers have also drained.
//   - processing_fence_t: the strongest guarantee - all pre-barrier message
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
/// unspecified order.  Barrier is an inter-process synchronisation mechanism -
/// combine it with traditional threading primitives if you also need
/// thread-level coordination within a process.
/// Barrier names beginning with `_sintra_` are reserved for internal runtime
/// protocols and will fail fast.
/// Returns the reply-ring watermark for the completed barrier, or
/// `invalid_sequence` when the barrier is treated as satisfied during
/// shutdown/drain handling.
template<typename BarrierMode = delivery_fence_t>
sequence_counter_type barrier(const std::string& barrier_name, const std::string& group_name = "_sintra_external_processes");

/// Returns true only when a barrier completed normally.
inline bool barrier_completed(sequence_counter_type barrier_sequence)
{
    return barrier_sequence != invalid_sequence;
}

// ---------------------------------------------------------------------------
// Shutdown options
// ---------------------------------------------------------------------------
/// Configuration for the standard coordinated shutdown protocol.
///
/// Ordinary callers use `shutdown()` for the common symmetric case.
/// When the coordinator must run a bounded side-effect (e.g. writing a
/// summary file) before raw teardown, pass a `shutdown_options` with a
/// `coordinator_shutdown_hook`.  Non-coordinator processes wait inside
/// the standard shutdown path while the hook is active.
///
/// Supported non-happy-path shutdowns should be expressed through additional
/// fields here, rather than through a parallel family of helper names.
/// Introduce a separate public helper only if the operation is no longer
/// semantically just shutdown.
struct shutdown_options {
    /// Optional callback that runs on the coordinator during shutdown, after
    /// the collective processing fence completes and before raw teardown
    /// begins.  The hook is coordinator-local and must not initiate new peer
    /// coordination, extra barriers, or additional custom protocol steps.
    ///
    /// If the hook throws, shutdown fails and the exception is surfaced to
    /// the caller.  If it blocks indefinitely, peers will not silently drift
    /// into unrelated teardown paths.
    std::function<void()> coordinator_shutdown_hook;
};

// ---------------------------------------------------------------------------
// Standard lifecycle terminal API
// ---------------------------------------------------------------------------
///\brief Perform the standard coordinated multi-process shutdown sequence.
///
/// `shutdown()` is the single terminal API for the standard lifecycle.
/// All live participants that use this path enter the same collective
/// shutdown protocol; the runtime owns the internal synchronization.
///
/// The simple form is appropriate when:
/// - all participants reach the same top-level handoff
/// - no important final side effect remains
/// - teardown may begin immediately
///
/// For an intentional unilateral departure where peers may continue running,
/// use `leave()`. For single-process programs, exceptional/error paths, or
/// deliberate primitive-level work in tests and experiments, use
/// `detail::finalize()`.
///
/// Ordinary callers should not pair `shutdown()` with extra final
/// `_sintra_all_processes` barriers or a direct subsequent `detail::finalize()`.
bool shutdown();

///\brief Perform a clean local departure without entering collective shutdown.
///
/// Use `leave()` when this process is intentionally exiting and peers may
/// continue running. This is the public form of the local drain-and-unpublish
/// lifecycle path used by leaf-like participants.
///
/// Coordinator processes may call `leave()` only when they are already the
/// sole remaining known process.
/// `leave()` must be initiated from a top-level control thread, not from a
/// message handler or post-handler callback.
bool leave();

///\brief Shutdown with options (e.g. a coordinator-side shutdown hook).
///
/// All participants call `shutdown(options)`.  If a coordinator_shutdown_hook
/// is configured, non-coordinator processes wait inside the standard shutdown
/// path while the coordinator runs the hook.  The library owns the necessary
/// internal synchronization.
///
/// The hook runs at a defined point within shutdown before raw teardown
/// begins.  It is coordinator-local and must not initiate new peer
/// coordination.
bool shutdown(const shutdown_options& options);


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


///\brief Block until a message of the specified type is received.
///
/// This is the synchronous receiving counterpart to the asynchronous send via
/// `world() << msg`.  It activates a temporary slot, blocks until a matching
/// message arrives, then returns the message payload.
///
/// Example usage:
/// \code
///     // Wait for a Stop signal (empty message)
///     receive<Stop>();
///
///     // Wait for a data message and capture its contents
///     auto msg = receive<DataMessage>();
///     std::cout << msg.value << std::endl;
///
///     // Wait for message from a specific sender
///     auto msg2 = receive<DataMessage>(some_sender_id);
/// \endcode
///
/// \warning Do not call this from within a message handler callback.  The
///          calling thread would block waiting for a message that it is
///          responsible for dispatching, causing a deadlock.  Use this only
///          from main process threads (e.g., inside process entry functions).
///
/// \note MESSAGE_T should be a trivial, standard-layout type (POD).  Messages
///       containing variable-length fields (message_string, variable_buffer)
///       are copied by value; ensure the returned data is used before any
///       subsequent message processing that might invalidate ring memory.
///
/// \tparam MESSAGE_T The message type to wait for (must be copy-constructible).
/// \return The received message payload.
template <typename MESSAGE_T>
MESSAGE_T receive();

///\brief Block until a message of the specified type is received from a specific sender.
///
/// \tparam MESSAGE_T The message type to wait for (must be copy-constructible).
/// \tparam SENDER_T  The sender type for filtering.
/// \param sender_id  The sender to filter messages from.
/// \return The received message payload.
template <typename MESSAGE_T, typename SENDER_T>
MESSAGE_T receive(Typed_instance_id<SENDER_T> sender_id);


///\brief Enable automatic recovery for the current managed process.
///
/// When recovery is enabled the coordinator will respawn the process if it
/// exits unexpectedly.  Applications that rely on clean shutdown should use
/// `sintra::shutdown()` as the standard terminal path.
void enable_recovery();

///\brief Configure recovery/lifecycle callbacks (effective only in coordinator).
void set_recovery_policy(Recovery_policy policy);
void set_recovery_runner(Recovery_runner runner);
void set_lifecycle_handler(Lifecycle_handler handler);

} // namespace sintra


// -- Runtime utilities ------------------------------------------------------
// High-level synchronisation, logging, message composition helpers, and the
// process runtime live here.  Historically these were gathered in
// `detail/sintra_impl.h`; we now include the specific headers directly to keep
// dependency relationships obvious.
#include "detail/barrier.h"
#include "detail/logging.h"
#include "detail/console.h"
#include "detail/maildrop.h"
#include "detail/runtime.h"

