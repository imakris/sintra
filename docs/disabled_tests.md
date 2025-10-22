# Disabled stress choreography tests

Sintra ships a handful of long-running choreography/stress programs that are
commented out from `tests/CMakeLists.txt`.  The following notes capture why each
program currently fails when executed directly so that the disabled state is
explicit.

## `barrier_pathological_choreography_test`

Each worker joins the per-slot barriers using `_sintra_all_processes`, but the
controller process keeps the default `_sintra_external_processes` group.  As a
result the workers wait for the starter process, which never participates in the
slot-specific barriers, and the run deadlocks during the first final-stage
barrier.  Aligning the group selection with the controller unblocks the test
(the workers should call the default overload). 【F:tests/barrier_pathological_choreography_test.cpp†L473-L498】

## `choreography_extreme_test`

Re-enabling the instrumentation shows that the aggregator stops receiving worker
updates after broadcasting `StartPhase 1`; only the first worker submits the
first round before the choreography stalls.  The log excerpt below is captured
from `/tmp/sintra_tests/extreme_choreography_*` after launching the binary in
isolation. 【a56ac8†L1-L22】

```
Aggregator slots ready barrier
Aggregator passed slots-ready
PhaseAnnouncement 0 rounds=3
...
PhaseComplete 0
PhaseAnnouncement 1 rounds=5
StartPhase 1
```

Workers in `run_producer` gate their progress on `start_cv.wait(...)` and never
recover if the `StartPhase` broadcast is processed before the matching
`PhaseAnnouncement` updates their `current_phase`.  The controller fires both
messages back-to-back, so the choreography depends on delivery order and remains
stuck when the scheduling differs. 【F:tests/choreography_extreme_test.cpp†L620-L707】【F:tests/choreography_extreme_test.cpp†L322-L368】

## `complex_choreography_stress_test`

Executing the stress binary aborts almost immediately with
`std::runtime_error: Attempted to make an RPC call using an invalid instance ID`,
indicating that at least one participant tears down its process registration
before the remaining actors finish their RPC/barrier sequence. 【2a5e11†L1-L24】
The worker loop expects every `Phase_command` to arrive promptly and gives up
when the command does not match the token it is waiting for, while the conductor
continues broadcasting commands for the next round.  The resulting invalid-state
notifications cascade into the crash. 【F:tests/complex_choreography_stress_test.cpp†L552-L625】【F:tests/complex_choreography_stress_test.cpp†L728-L784】
