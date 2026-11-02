# P11: Sintra RPC, locks and lifecycle

## Outcome and continuation

Implemented the supported coordinator-route cancellation and named-container
caller-attribution corrections. Completed bounded Linux investigations for the
other assigned findings, with explicit Windows/macOS follow-up. The draft is
[PR #876](https://github.com/imakris/sintra/pull/876), branch
`work/linux-review-20261102-p11-sintra`.

- Source baseline: `dbdd0ecdb972bd8c924272a6c3be879fdf4e46fd`.
- Tested production candidate: `615cf1485979a8b829f3a23a164d36a50401445b`.
- The initial report was published before implementation and remaining gates;
  subsequent caller-site, fixture/measurement and RPC commits preserve progress.
- The canonical checkout remains clean on `master`. The task uses a dedicated
  worktree; there was no pre-existing dirty state to overwrite.
- Next action: review this draft for integration, then execute the precise native
  Windows retention and macOS toolchain checks below. Ring/handler wait-policy
  changes are separate design decisions; no such production change is included.

Two fresh independent highest-reasoning read-only reviewers examined both
production proposals before adoption. Both reported **NO BLOCKERS** for each.
Subsequent independent implementation reviews are also clean. The RPC reviewer
identified a result-file publication race in the fixture; it was repaired with a
content-aware wait before final candidate execution. Reviews establish source
and design findings, not unexecuted platform certification.

The owner requirements remain the package scope and
[Sintra design principles](../../design_principles.md). Verified source facts,
executed measurements and proposed follow-up are distinguished below. Historical
source confirmation is not silently upgraded to native application reproduction.

## Finding dispositions

| Finding | Final disposition | Evidence and limits |
|---|---|---|
| **35: peer RPCs after coordinator loss** | **Fixed for the existing terminal coordinator-reader stop path**, including concurrent late registration. | The real external-leaf fixture failed before the RPC fix, then passed three candidate runs. Covers peer async/sync calls, unrelated peer retirement, completed replies, direct local calls, strict local calls and external-leaf cleanup. No new detector for unnotified abrupt coordinator death. |
| **116: Windows semaphore cache retention** | **Source-confirmed; borrower/lifetime trace and native fixture delivered.** No reclamation change. | Cache holds raw handles indefinitely, and active operations plus admission/token lifetime constrain closing them. Windows execution and real per-child handle counts remain unavailable. Linux explicitly returns unavailable status 77 for the native probe. |
| **164: writer wait on a live reader guard** | **Measured controlled CPU cost; delivery/eviction checks pass.** No spin-policy change. | A deliberately held guard uses approximately 5.1–5.2 ms of writer CPU per forced eviction in the single-worker fixture. Oversubscribed trials preserve exact eviction counts and post-gap delivery. This does not measure application throughput or pending-guard amplification. |
| **117: wrapper caller attribution** | **Fixed for the named container methods and scoped access.** | Original enabled fixture reported the wrapper for all 12 baseline cases. Both corrected source-location modes pass 13 cases, including explicit forwarding. Assignment/conversion and variadic `push_back` retain their existing attribution limitation. |
| **165: foreign-owner probes** | **Bounded paired measurement delivered; no demonstrated need for a cadence change.** | Real same/foreign-process holders each produced six poll opportunities during a requested 30 ms hold. Wall/CPU ranges overlap. Poll opportunities are not exact syscall counts; native dead-owner recovery remains covered by the maintained tests. |
| **118: handler deactivation wait** | **Measured near-full-thread CPU during a deliberately blocked external callback.** No wait replacement. | A roughly 100 ms external wait consumed 99.72–100.26 ms of thread CPU. Existing external/self-deactivation assertions pass, including under four competing workers. Application incidence and a safe replacement's latency/ordering still need evidence. |
| **166: macOS header floor** | **Finite source investigation and exact native consumer gate supplied.** No floor/guard change. | Unconditional `<stop_token>` inclusion and feature-guarded APIs remain inconsistent in shape. Actual Xcode 15 headers were not available. `<format>` and mandatory Apple wait/clock headers must also be checked before attributing a declared-floor failure. |

## Implemented behavior and rationale

### Coordinator reply-route cancellation (35)

Baseline `process_message_reader_impl.h:442-459` cancels only calls whose target
matches the stopped reader. A leaf creates only its coordinator reader
(`managed_process_impl.h:3545-3555`), and nonlocal replies arrive through the
coordinator (`process_message_reader_impl.h:1081-1086`). A pending call targeting
another leaf therefore escaped the coordinator-targeted cancellation scan.

The correction recognizes exactly a leaf's coordinator reader. It publishes the
existing terminal `m_must_stop` state before cancelling all still-pending
transported calls. The coordinator's ordinary peer readers retain exact-target
cancellation. `rpc_async_impl()` checks that terminal state under the same
outstanding-registry mutex, immediately before insertion:

- A registration before the cancellation scan is seen by that scan.
- A registration after the scan observes the earlier stop publication through
  the shared mutex ordering and rejects before insertion/return-handler setup.
- A completed reply remains terminal and is not overwritten by cancellation.
- Non-strict blocking calls to an already-resolved local live target use their
  existing direct path. Strict local calls require transport and reject.

Dynamic message-type resolution remains outside the registry lock. Invocation
can now throw `rpc_cancelled` before returning an async handle when the leaf has
started stopping; both RPC reference pages document that behavior. Cancellation
still does **not** prove that a previously published remote request did not run.

`coordinator_loss_rpc_test` uses three real external leaves without lifelines.
It first retires one unrelated peer while another real RPC remains pending and
then completes. Its route-loss phase invokes actual coordinator self-unpublication
and the resulting deferred leaf stop. It holds a remote handler, retains an
unconsumed completed reply, and pauses another caller at the existing
pre-registration hook. Pending async/sync cancellation, late/new/strict-local
rejection, direct local success and actual leaf finalization are checked.

Before the RPC correction, the fixture failed in **1.219 s**: the async peer call
timed out, the synchronous caller needed rescue after recording failure, and
late/new-peer/strict-local rejection was absent. After the correction it passed
**three runs, approximately 0.217 s each**. Baseline isolation, completed-result
and direct-local assertions did not fail. The later result-file fix removes a
possible false failure from observing creation before the marker's contents.

This is a real routing/notification reproduction, **not a SIGKILL/death-detection
reproduction**. An abrupt coordinator death without a stop notification or
lifeline remains outside this correction. Soter recovery is not qualified.

### Container acquisition sites (117)

Baseline `spinlock.h:313-322` captures source location where the locker is
constructed. Container wrappers constructed it inside their own implementation.
The correction captures at named entry points and forwards that location through
scoped-access construction and into the existing locker.

Covered methods: mutable/const `scoped`, both scoped-access constructors,
`with_lock`, `clear`, `empty`, `pop_front`, `size`, `contains_key`, `copy_value`
and `set_value`. No owner/generation layout, synchronization, native death
proof, probe cadence, backoff or diagnostic policy changes.

The supported location type aliases `std::source_location`; a toolchain without
it uses the empty fallback. Tests exercise real nonfatal recovery diagnostics
using the existing same-PID/earlier-token hook, then check file/line and container
results. The fallback is a separate consistent header-only translation unit,
with feature suppression verified; it does not mix modes with the compiled core.
The existing native stderr capture helper is shared rather than duplicated.

Ordinary call syntax is preserved. Added defaulted arguments do change named
method pointer-to-member types; no corresponding repository caller was found.
Assignment/conversion operators and the unconstrained variadic `push_back`
retain their signatures and wrapper-site attribution. This is a bounded named
method correction, not complete attribution for every possible operator.

## Measured ring, spinlock and handler behavior

The following are native Linux component measurements with no compiler work
running concurrently. No timing or CPU threshold was added to the normal suite.
Manual probes are opt-in through `SINTRA_BUILD_MANUAL_TESTS=ON`.

| Command under `build/tests/` | Native result |
|---|---|
| `manual/sintra_manual_ring_resource_probe --stall 1 10` | Each held trial: 10 evictions, all 10 later markers delivered to the original readers, no accounting mismatch. Summed writer CPU **51.20–51.96 ms**, versus **0.0431–0.0433 ms** with guards released. Full trial wall **78.88–78.94 ms** held versus **23.27–28.84 ms** released. |
| `manual/sintra_manual_ring_resource_probe --stall 16 4` | 16 workers on 9 exposed CPUs: exactly 64 evictions and 64 post-gap deliveries per held trial. Summed writer CPU **311.40–319.94 ms** held versus **0.629–0.689 ms** released. Full trial wall **111.88–115.15 ms** versus **61.31–67.21 ms**. |
| `manual/sintra_manual_spinlock_contention_probe --measure` | All 12 protected-value checks pass. Thread/foreign-child holders keep a real mapped lock for 30 ms after the first waiter poll. Six poll opportunities per acquisition. Wall **30.20–30.42 ms** same-process and **30.23–30.39 ms** foreign; waiter CPU **2.06–4.49 ms** and **2.10–2.55 ms**, respectively. |
| `sintra_handler_lifetime_test --measure-deactivation` | Eight external and eight self-deactivation cases, in 0/4/4/0 competing-worker order. External wait wall **100.16–100.26 ms**, CPU **99.72–100.26 ms**. Self-deactivation call wall **0.0041–0.0052 ms**. Lifetime assertions pass throughout. |
| `sintra_mass_join_test` | All 40 workers join, exchange RPCs/broadcasts and finalize. Two quiet baseline runs report **526/506 ms**; the corresponding candidate pair reports **489/464 ms**. Candidate gate run also passes at 436 ms. These few samples do not establish a performance improvement. |

Ring trials reverse held/released order on their second pair. Their full trial
wall time includes mapping/file setup. Each worker has an independent ring;
this is CPU oversubscription, not a claim about sixteen readers on one ring.
The original reader resumes after eviction and receives the next marker, and
no obsolete snapshot is dereferenced.

The ring wait at baseline `rings.h:4423-4527` has no backoff on the ordinary
blocking-guard branch. The existing budget is 5,000 microseconds, but pending
publication/copying can span multiple passes. Not every episode is exactly
5 ms. `count_guards_for_octile()` probes native identity for **pending guards**
(`:2626-2629`); ordinary stable guards are counted atomically. These trials do
not measure that pending-guard amplification or application throughput.

Spinlock hooks count **poll opportunities**, not syscalls. Same-process
ownership skips the foreign-process native lookup. The measured ranges do not
support a cadence change, and the recent spin/sleep policy remains intact.
Native death/recovery behavior was separately exercised by the maintained
spinlock recovery and real-clock tests.

The handler fixture deliberately holds an external callback for about 100 ms
and stages queued self-dispatch for about 200 ms. The latter's full
invocation-to-deactivation duration is fixture choreography, not a slow
self-deactivation call. The controlled external wait demonstrates actual CPU
cost, while retaining the original admission, capture-lifetime and queued
self-dispatch assertions. A wait replacement still needs a reviewed
admission/wakeup/exception-unwind design and paired latency measurements; no
unmeasured production policy was adopted.

## Windows semaphore lifetime and exact continuation (116)

Source locations here refer to the baseline. The cache is deliberately never
destroyed (`semaphore.h:283-304`) and stores raw handles without erasure
(`:358-400`). Backend destruction (`:670-673`) and public
`release_local_handle()` (`:1125-1128`) are no-ops. Per-name retention is
source-confirmed; exact production per-child growth and impact are unmeasured.

| Borrower | Native operation outside the cache lock |
|---|---|
| `try_wait` | `semaphore.h:529-535` |
| Indefinite `wait` | `:549-555` |
| Timed wait | `:565-582` |
| `post` | `:598-605` |
| Quiescent reset | `:631-636` |
| Binary post | `:657-661` |

Erasing and immediately closing a handle can race any borrower. Per-call
retention alone also misses the token's lifetime: reader admission pins the
semaphore before publishing its slot (`rings.h:3034-3040`), and binary posting
relies on that pin. Closing the last native handle between posting and waiting
can destroy a pending token. Retirement drains/reset-checks the token before
slot reuse (`:3095-3124`). Shared control destruction runs only at the globally
last detach (`:2790-2813`), possibly in another process, and is not a complete
process-local cache ownership boundary. Multiple local mappings can share one
name; late static cleanup still needs handles to unblock and join readers.

**Direction for a future proposal, not an adopted design:** retain native
operation borrowers and explicitly represent local mapping/admission ownership.
Only the last local owner plus last borrower can make closing eligible. Preserve
pending-token identity, aliases, admission-to-quiescent-retirement pinning and
late cleanup. Trace public semaphore users as well as rings, then obtain the
required two independent clean design reviews before reclamation changes.

The prepared `ring_resource_probe --handle-retention 64` compares a warmed reused
ring against fresh ring names, verifies payloads and cached handle validity, and
reports native process-handle and cache-name deltas. It is header-only so its
internal-linkage cache counter observes the same cache used by its calls. Linux
execution returned **77, explicitly unavailable**; this is not a native pass.

On Windows, record the exact compiler, native backend, source revision and full
paired counts. Run semaphore, wakeup-ownership and ring lifecycle gates. Then
measure repeated real managed-child admission/normal detach/exit with a surviving
coordinator, sampling after reader joins and child-handle retirement. Include
concurrent wait/post/reset, multiple local mappings, pending-token transfer and
static cleanup before adopting reclamation. The raw-ring probe does not quantify
a managed child or qualify Windows lifecycle behavior.

## macOS header-floor investigation and exact gate (166)

Baseline `README.md:265-267` declares macOS 15.0 and Command Line Tools for Xcode
15 or newer. CMake defaults deployment target to 15.0; that is distinct from SDK
header availability. `transceiver.h:19` unconditionally includes `<stop_token>`;
`:1007-1012` and `transceiver_impl.h:1121-1150` guard the API with
`__cpp_lib_jthread >= 201911L`. The full header consumer also needs `<format>`
(`detail/type_utils.h:9`, `detail/messaging/message.h:18`) and Apple's
`<os/os_sync_wait_on_address.h>` / `<os/clock.h>` (`semaphore.h:107-112`).

No actual Apple SDK/toolchain was available here. Neither a successful Linux
include nor missing Apple headers on Linux can establish the declared floor.
The minimal native consumer is:

```cpp
#include <sintra/sintra.h>
int main() { return 0; }
```

Run it against the unchanged baseline and any proposed candidate, first with
actual Xcode 15 Command Line Tools and then the current toolchain:

```bash
xcode-select -p
xcrun --sdk macosx --show-sdk-version
xcrun --sdk macosx clang++ --version
queued-build --slots 1 -- xcrun --sdk macosx clang++ \
  -std=c++20 -mmacosx-version-min=15.0 -I "$SINTRA_SOURCE/include" \
  -fsyntax-only sintra_header_consumer.cpp
```

Keep the complete first diagnostic and SDK identity. Check individual headers
and the feature macro only after the full-consumer result. A stop-token guard
alone would not prove compatibility with the declared floor. No floor or header
guard is changed by this PR.

## Validation actually performed

| Gate | Actual result |
|---|---|
| Package integrity | All 11 manifest-listed members match byte sizes and SHA-256. |
| Governing source/instructions | Current clone, `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `TESTING.md`, design principles and current Linux SDK guidance inspected. |
| SDK | Release checksum and pinned digest `56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2` match. Preflight passed after local launcher repair described below. |
| Native baseline | **59/59 passed**: all 50 raw-ring subcases plus RPC async lifecycle, reader-stop ownership, finalize lifecycle, semaphore, wakeup ownership, spinlock recovery/real clock, mass join and handler lifetime. |
| Baseline regression proof | Caller-site enabled fixture failed all 12 original location checks; fallback passed. New coordinator-loss fixture failed the five expected cancellation/rejection checks against unchanged RPC headers. |
| Final candidate | **10/10 passed** on tested production commit `615cf148`: coordinator loss, RPC async lifecycle, reader-stop ownership, finalize async lifecycle, spinlock recovery, real clock, mass join, handler lifetime, and both caller-site modes. |
| Deterministic cancellation repeat | Two further coordinator-loss runs passed: **3/3 total candidate executions**, all about 0.217 s. |
| Measurement modes | All bounded ring, spinlock and handler assertions passed; native values above. Separate baseline/candidate mass-join pairs passed. |
| Cache and build queue | Compiler-invoking configuration reserved one slot; focused builds used at most four with matching native width. Persistent `ccache` was active, ending with 34 cacheable calls and 3 preprocessed hits. |
| Native Windows/macOS | Not executed. Windows-only probe explicitly reports unavailable on Linux. |
| Integrity/publication | `git diff --check` clean; connected-GitHub publication checked every created tree against the corresponding local committed tree. No force-push or default-branch landing. |

The toolchain was Ubuntu 24.04 x86-64, GCC 13.3.0, CMake 3.31.10 and Ninja 1.13.2,
using the pinned `logonomic-linux-development-sdk-2026.10.23.1` tools. The project
was standalone Release, tests/manual targets enabled, examples disabled, release
debug symbols disabled, and `CMAKE_CXX_COMPILER_LAUNCHER=ccache`. No Qt-dependent
application target or SDK-owned product binary was used. Source includes point
to the current dedicated Sintra worktree. Ring, spinlock and handler policy
measurements use their recorded pre-fix binaries; those measured wait policies
are unchanged by this PR.

The selected native executables were run serially through the maintained
`TestRunner.find_test_suites()` / `run_test_once()` harness with a 30-second base
bound and its existing per-test overrides. The ring suite expands to 50 cases.
Only the installed-package gate is a registered CTest entry in this checkout;
no empty CTest selection was credited. This is focused Release coverage, not a
full Debug/Release stability schedule or cross-platform certification.

### Reproducing the focused candidate checks

After SDK activation and selecting persistent writable cache/queue directories,
configure the standalone checkout and build only the selected targets:

```bash
queued-build --slots 1 -- cmake -S . -B ../build-p11 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSINTRA_BUILD_TESTS=ON \
  -DSINTRA_BUILD_EXAMPLES=OFF -DSINTRA_BUILD_MANUAL_TESTS=ON \
  -DSINTRA_RELEASE_WITH_DEBUG_SYMBOLS=OFF -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
queued-build --slots 4 -- cmake --build ../build-p11 --parallel 4 --target \
  sintra_coordinator_loss_rpc_test sintra_rpc_async_lifecycle_test \
  sintra_process_reader_stop_ownership_test sintra_finalize_async_rpc_lifecycle_test \
  sintra_spinlock_recovery_test sintra_spinlock_real_clock_test sintra_mass_join_test \
  sintra_handler_lifetime_test sintra_spinlocked_caller_site_test \
  sintra_spinlocked_caller_site_no_source_location_test
```

From the checkout, the exact maintained-harness selection can be reproduced
without altering the normal active-test roster:

```python
from pathlib import Path
import sys
sys.path.insert(0, str(Path("tests").resolve()))
from run_tests import TestRunner
names = """coordinator_loss_rpc_test rpc_async_lifecycle_test
process_reader_stop_ownership_test finalize_async_rpc_lifecycle_test
spinlock_recovery_test spinlock_real_clock_test mass_join_test
handler_lifetime_test spinlocked_caller_site_test
spinlocked_caller_site_no_source_location_test""".split()
runner = TestRunner(Path("../build-p11").resolve(), "Release", 30, False)
suites, _ = runner.find_test_suites(dict.fromkeys(names, 1))
assert sum(map(len, suites.values())) == len(names)
for suite in suites.values():
    for invocation in suite:
        result = runner.run_test_once(invocation)
        print(invocation.name, result.result_kind, result.duration)
        if not result.success or result.result_kind != "pass":
            raise RuntimeError(result.output + result.error)
```

The resource measurements additionally build
`sintra_manual_ring_resource_probe` and `sintra_manual_spinlock_contention_probe`
under the same queue. Use the maintained process-owning harness or an external
bound for optional manual modes. Native PID/procfs coordinates must agree.

### Environment and build failures retained in the evidence

The initial default sandbox returned PID 6 while `/proc/self/stat` identified
18472, and `/proc/6/stat` had another start time. This violates design principle
6. It can cause lifecycle scavenging to treat a live attachment as a different
incarnation and recreate backing files. Two orderly replacement-writer
assertions failed, a snapshot case timed out and wakeup ownership timed out.
The assertion cases entered the diagnostic pause; their timeout was not a
separate proof of cleanup deadlock. The identical binaries passed all 59
invocations in the matching native namespace, with no production or expectation
change. All credited performance measurements were rerun there.

The verified SDK archive lacked `bin/cmake`, `bin/ctest`, `bin/cpack` and
`bin/ninja` launchers even though their binaries were present under `tools/`.
Local relative launchers to those bundled binaries restored preflight; no SDK
source or preflight rule was changed. Optional SQL/SpatialAudio plugins reported
missing dependencies; none is consumed by these standalone tests.

One candidate link failed because the mass-join object file was **zero bytes**,
producing an undefined `main`. The source still contained `main`; the object's exact creation cause remains
unestablished. Only that build artifact
was removed, and the same source/compiler was rebuilt in the native profile with
its cache entry refreshed. The object became 2,381,416 bytes, the final focused
build succeeded, and all candidate/mass-join executions passed. This transient
build failure is retained rather than relabelled a passing first build.

No private hosted workflow was dispatched, and no live-user UI, native device,
application recovery or release qualification was performed.
