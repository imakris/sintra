# P11: Sintra RPC, locks and lifecycle

## Outcome and continuation

The draft PR is [#876](https://github.com/imakris/sintra/pull/876), on branch
`work/linux-review-20261102-p11-sintra`, based on
`dbdd0ecdb972bd8c924272a6c3be879fdf4e46fd`. The canonical checkout remains clean
on `master`; there was no pre-existing task work to overwrite.

Two fresh, independent, highest-reasoning read-only reviewers examined the RPC
and caller-attribution designs separately. Both reported **NO BLOCKERS** for
both proposals before implementation. A subsequent independent implementation
review also found no blocker in the caller-attribution change. These are design
and source-review results, not native-platform certification.

The named-container attribution correction is implemented and its enabled and
disabled source-location fixtures pass. The coordinator-loss fixture reproduces
five cancellation/admission failures against the unchanged RPC implementation;
the reviewed two-file correction is the next step. Optional ring, spinlock and
handler measurements have run. Windows handle reclamation and the macOS header
floor remain native follow-up; no wait-policy change is included.

This checkpoint preserves implementation and fixtures while final RPC correction
and candidate gates continue. Native baseline gates: **59/59 passed**, including
all 50 raw-ring cases. The exact same binaries initially produced four failing
invocations in an unsupported PID/procfs sandbox; all passed after moving to a
matching native namespace, without a production or expectation change.

The owner requirements are the package's scope and
[Sintra design principles](../../design_principles.md). The code observations
below are verified baseline facts. Proposed remedies are explicitly identified;
historical review severity and native evidence are not upgraded by inspection.

## Finding dispositions

| Finding | Current evidence at the source baseline | Disposition at this checkpoint | Remaining uncertainty |
|---|---|---|---|
| 35: peer RPCs after coordinator loss | `Process_message_reader::unblock_rpc_once()` calls `unblock_rpc(m_process_instance_id)`; leaves own only the coordinator reader. Peer replies use that reader, although their pending controls target their peer. | Cancellation omission remains source-confirmed. Investigate terminal route state and concurrent admission before choosing a fix. | Exact executed reproduction and race/isolation tests pending; lifeline-driven hard exit normally masks the defect. |
| 116: Windows semaphore cache retention | `ips_win_handles()` intentionally survives static teardown; per-name raw `HANDLE`s are cached without erasure. Operations use those handles after releasing the cache mutex. | Retention is source-confirmed. No handle reclamation change is justified by Linux execution. Trace both temporary borrowers and ring-token ownership before a proposal. | Actual native handle growth, resource impact and safe reclamation need Windows execution. |
| 164: writer wait on a live reader guard | The octile-advance loop repeatedly scans blocking guards; yields in the guardless branch do not cover the ordinary live-guard branch. | The loop structure remains; prepare a bounded stalled-reader component measurement. No spin-policy optimization is adopted. | Writer CPU, delivery, evictions and full-path throughput have not yet been measured. Native probes are conditional, not one per guard scan. |
| 117: wrapper caller attribution | Container methods construct `spinlock::locker` inside `spinlocked_containers.h`, where its default source location is captured. | Incorrect wrapper-site attribution remains source-confirmed. Prepare explicit forwarding at ordinary method boundaries and a nonfatal diagnostic fixture. | The source-location-disabled build and methods whose signatures cannot accept default location arguments need explicit treatment. |
| 165: foreign-owner probes | `next_poll` starts at `wait_start`; eligible misses enter native recovery probing immediately and later at the existing 5 ms cadence. Current code also includes the recently merged spin/sleep backoff. | Preserve the current policy while measuring its conditional cost. The older claim is not evidence of application-level regression. | Probe count, CPU and acquisition/mass-join latency require runtime evidence; same-process ownership is a distinct path. |
| 118: handler deactivation wait | `Transceiver::activate_impl()`'s deactivator yields until active invocations drain, except its own self-deactivating invocation. Admission uses an SC ordering and per-slot invocation mutex. | Investigation-first: retain admission ordering and self-deactivation; measure the external wait with the existing lifetime contract. | A yield loop alone does not establish one full core of CPU use or justify a replacement primitive. |
| 166: declared macOS header floor | `transceiver.h` unconditionally includes `<stop_token>` while stop-token APIs are feature-guarded; the documented floor is macOS 15 and Xcode Command Line Tools 15. Other mandatory headers also constrain that floor. | Source inconsistency recorded. Keep the floor and guards unchanged pending actual baseline-toolchain evidence. | No Apple SDK/compiler exists on this Linux host; neither `<stop_token>` availability nor the declared floor is native-validated here. |

## Source evidence and constraints

All locations in this section refer to the baseline above.

### RPC route loss (35)

`include/sintra/detail/messaging/process_message_reader_impl.h:442-459`
contains the once-only, reader-target cancellation.
`include/sintra/detail/process/managed_process_impl.h:3545-3555` creates the
coordinator reader in every process. The leaf has no separate peer reader to
cancel peer-targeted pending calls when the relay disappears.

The proposed change must also account for registrations racing with the
cancellation scan. It must preserve ordinary locally satisfiable direct RPCs
and retain target-specific behavior when the coordinator loses an unrelated
leaf. The finding does not authorize blanket cancellation on arbitrary leaf
loss, nor does it qualify Soter recovery.

### Semaphore lifetime and writer wait (116, 164)

`include/sintra/detail/ipc/semaphore.h:297-304` explains why the cache itself
survives exit-time cleanup. `:358-400` returns cached raw handles and inserts
new names without an erase path. For example, `:529-555` uses a returned handle
for a native wait outside the cache lock. Erasing a map entry and immediately
closing its handle can race that borrower. Ring admission also pins its
wakeup token before publishing the slot (`rings.h:3034-3040`), so a safe future
design must cover token identity/lifetime, not only the duration of a native
wait call.

`include/sintra/detail/ipc/rings.h:4418-4455` distinguishes a blocking live
guard from guardless recovery. The latter contains yields; the former can
rescan continuously until the existing eviction path resolves it. This does
not establish the frequency or cost of native liveness probes, which occur
on pending-guard scans and in conditional reclamation paths. Ordinary stable
guards are counted without native probing (`rings.h:2626-2629`).

### Handler lifetime (118)

`include/sintra/detail/transceiver_impl.h:509-542` first closes admission,
unregisters the handler, and then waits. The allowed invocation count is one
only for the currently executing handler itself. In
`include/sintra/detail/messaging/process_message_reader_impl.h:160-183` and
`:240-267`, the invocation scope counts an admitted callback and preserves the
final active check. `tests/handler_lifetime_test.cpp` already covers an
externally blocked handler and queued dispatch during self-deactivation.

Any future wait replacement must retain that ordering, permit self-deactivation,
wait for external invocations before releasing captures, and avoid holding a
transition lock across a callback that can itself deactivate.

### macOS header floor (166)

The actual declared minimum is in `README.md:265-267`: macOS 15.0 and Command
Line Tools for Xcode 15 or newer. `CMakeLists.txt` defaults the deployment
target to 15.0. Deployment target and SDK/header availability are separate
requirements.

`include/sintra/detail/transceiver.h:19` includes `<stop_token>` unconditionally;
`:1007-1012` and `transceiver_impl.h:1121-1150` guard the optional API with
`__cpp_lib_jthread >= 201911L`. The complete consumer also needs `<format>`
(`detail/type_utils.h:9`, `detail/messaging/message.h:18`) and, on macOS,
`<os/os_sync_wait_on_address.h>` and `<os/clock.h>`
(`detail/ipc/semaphore.h:107-112`). A conditional stop-token include alone
would not establish that this declared toolchain can consume Sintra.

The exact first native gate is this minimal consumer, built against both the
unchanged baseline and any candidate, under the actual declared Xcode 15
Command Line Tools and then the current macOS toolchain:

```cpp
#include <sintra/sintra.h>
int main() { return 0; }
```

```bash
xcode-select -p
xcrun --sdk macosx --show-sdk-version
xcrun --sdk macosx clang++ --version
queued-build --slots 1 -- xcrun --sdk macosx clang++ \
  -std=c++20 -mmacosx-version-min=15.0 -I "$SINTRA_SOURCE/include" \
  -fsyntax-only sintra_header_consumer.cpp
```

Keep the complete first diagnostic and SDK identity. Check individual header
availability and `__cpp_lib_jthread` only after recording the full-consumer
result. A Linux successful include, or a Linux missing Apple header, cannot
validate or refute the macOS floor. No floor/guard change is included here.

## Validation actually performed

| Check | Inputs/environment | Actual result |
|---|---|---|
| Package integrity | All 11 manifest-listed payload members in `P11_sintra.zip` | Exact byte sizes and SHA-256 hashes match. |
| Current source refresh | Fresh public Git clone at `dbdd0ecd` | Clean `master`; task worktree created without pre-existing modifications. |
| Governing instructions | `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `TESTING.md`, `docs/design_principles.md`; current Logonomic Linux SDK guidance | Read before proposing changes. Task-specific draft publication and focused gates govern this assignment. |
| Source revalidation | Owner modules and maintained focused tests | Observations above reproduced by inspection; not runtime proof. |
| SDK archive | `logonomic-linux-development-sdk-2026.10.23.1` | Release `SHA256SUMS` and pinned SHA-256 `56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2` match. Preflight passed after adding missing local launchers pointing to the already bundled CMake 3.31.10, CTest/CPack and Ninja 1.13.2 binaries. No SDK source was changed. |
| Build configuration | Ubuntu 24.04 x86-64, GCC 13.3, CMake 3.31.10, Ninja 1.13.2; standalone Release, examples off, manual targets on, debug symbols off | Compiler-invoking configuration used one queue slot; explicit focused targets used four slots and `--parallel 4`. `ccache` launcher and persistent task cache were active. No Qt-dependent target was built. |
| Baseline focused execution | Native PID/procfs coordinates checked before execution; maintained `TestRunner.run_test_once`, serial | 59/59 passed: 50 `ipc_rings_tests` subcases plus RPC lifecycle, reader-stop ownership, finalize lifecycle, semaphore, wakeup ownership, spinlock recovery/real clock, mass join and handler lifetime. |
| Caller-site reproduction/candidate | Two consistent header-only fixtures | Original enabled fixture failed all 12 location assertions while recovery/operations passed; fallback passed. Corrected enabled/fallback fixtures pass all 13 cases including explicit forwarding. |
| Coordinator-stop reproduction | Three actual external leaves, no lifeline; unchanged RPC headers | Failed in 1.219 s: async peer timed out, sync peer needed rescue, and late/new-peer/strict-local invocation rejection was absent. The fixture records failure before cleanup rescue. Candidate pending. |
| CTest registration | `ctest --show-only=json-v1` | Only the installed-package gate is a CTest entry. The selected native executables were run through Sintra's maintained Python harness; no empty CTest selection is credited. |

## Native and otherwise pending follow-up

- Windows: warmed repeated ring attach/detach, repeated child admission/exit,
  handle/cache growth, in-flight borrowers and static teardown. Exact retained
  handle count and practical impact remain unmeasured.
- macOS: the minimum/current-toolchain full-header consumer above. Preserve
  any unrelated prerequisite diagnostic rather than crediting a partial check.
- Linux: the focused RPC/reader-stop, ring/semaphore/wakeup,
  spinlock/real-clock/mass-join and handler-lifetime tests, plus the bounded
  optional measurement modes. Record actual registrations; Sintra's test
  executables are not necessarily CTest entries.
- Design adoption: the two required independent reviewers have both reported no
  blockers for the RPC and named-caller proposals. New production wait/lifetime
  designs would require their own reviews.

No private hosted workflow is dispatched by this assignment, and no live-user
UI, native device or application recovery qualification is performed.


## Executed optional measurements

These are bounded component observations on this host, not application
throughput or cross-platform performance qualification. No performance threshold
was added to the normal suite, and no probe/backoff/wait policy was changed.
Manual probes are opt-in through `SINTRA_BUILD_MANUAL_TESTS=ON`.

| Command (under `build/tests/`) | Native result |
|---|---|
| `manual/sintra_manual_ring_resource_probe --stall 1 10` | Each held-guard trial evicted exactly 10 times and delivered all 10 later markers to the original readers. Summed writer CPU: 51.20–51.96 ms, versus 0.0431–0.0433 ms with guards released. Full trial wall time: 78.88–78.94 ms versus 23.27–28.84 ms; file/mapping setup is included in that latter metric. |
| `manual/sintra_manual_ring_resource_probe --stall 16 4` | 16 workers on 9 exposed CPUs: exactly 64 evictions and 64 post-gap deliveries per held trial, no guard accounting mismatch. Summed writer CPU: 311.40–319.94 ms held versus 0.629–0.689 ms released. Full trial wall: 111.88–115.15 ms held versus 61.31–67.21 ms released. |
| `manual/sintra_manual_spinlock_contention_probe --measure` | All 12 protected-value checks pass. A real thread or foreign child holds the mapped lock for 30 ms after the first waiter poll. Each acquisition observed six poll opportunities. Acquisition wall: 30.20–30.42 ms same-process and 30.23–30.39 ms foreign; waiter CPU: 2.06–4.49 ms and 2.10–2.55 ms respectively. |
| `sintra_handler_lifetime_test --measure-deactivation` | Eight external waits and eight self-deactivations pass the existing lifetime assertions, with 0/4/4/0 contention workers. External wait wall: 100.16–100.26 ms; thread CPU: 99.72–100.26 ms. Self-deactivation itself: 0.0041–0.0052 ms wall, preserving queued-invocation suppression. |
| `sintra_mass_join_test` (two quiet baseline runs) | All 40 workers join, exchange RPCs/broadcasts and finalize. Reported exchange times: 526 ms and 506 ms. Candidate comparison pending; two observations are not a performance distribution. |

The ring fixture measures deliberately held ordinary guards, not pending-guard
probe amplification or application throughput. The original reader receives a
marker after the gap; the probe never dereferences a snapshot after eviction.
Spinlock hooks count **poll opportunities**, not exact native syscall counts.
The same-process ownership fast path skips foreign-process liveness lookup;
the observed overlap does not justify changing native probe cadence.

The handler fixture deliberately holds an external callback for about 100 ms
and stages queued self-dispatch for about 200 ms. Its external deactivation
therefore demonstrates a real CPU cost for that controlled blocked callback;
the full invocation-to-deactivation time of about 200 ms in the self case is
fixture choreography, not a slow self-deactivation call. A replacement wait
still needs its own admission/wakeup/exception-unwind design and latency review.

The Windows-only `--handle-retention 64` mode compares warmed repeated
attach/detach of one ring with fresh ring names, checks delivery and live cached
handles, and reports `GetProcessHandleCount` plus cache-name counts. It has not
run on Windows. It is a raw-ring fixture, not a repeated managed-child test.

## Why the first sandbox runs are not native evidence

The default sandbox returned a PID of 6 while `/proc/self/stat` identified
18472; `/proc/6/stat` described another start time. This violates design
principle 6's explicit PID/procfs coordinate requirement. Lifecycle attachment
scavenging can then mistake a live attachment for a different incarnation and
recreate its backing files. Two orderly replacement-writer assertions failed,
one snapshot test timed out, and wakeup ownership timed out. The replacement
assertions entered the diagnostic pause; their timeouts were not separate
proof of cleanup deadlock.

The native execution profile was checked for matching PID and start-stamp
coordinates. **The identical binaries then passed all 59 selected invocations**,
including those four. No test expectation, production identity rule or ring
ownership logic was changed to accommodate the unsupported sandbox. All timing
numbers above are from fresh runs in the matching native profile, with no
compiler work running concurrently. The native checks used owned isolated
fixtures only.
