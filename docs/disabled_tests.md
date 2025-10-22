# Disabled stress tests

Sintra ships with several extreme or stress-oriented test programs that are not
part of the default test suite because they take minutes to finish or require
large amounts of scheduling luck.  They are useful for manual testing but are
not a good fit for automated CI runs.

## `choreography_extreme_test`

* Location: `tests/choreography_extreme_test.cpp`
* Behaviour: orchestrates six managed processes across four phases with heavy
  coordination and audit traffic.
* Observed runtime: a manual run with the release build takes well over a
  minute; a run was interrupted after ~105 seconds with active worker processes
  still in flight.【F:tests/choreography_extreme_test.cpp†L19-L120】【2be456†L1-L6】【82f416†L1-L3】
* Recommendation: keep disabled by default. Run manually with a generous
  timeout (e.g. `timeout 300 ./sintra_choreography_extreme_test_release`) when
  investigating performance regressions.

## `complex_choreography_stress_test`

* Location: `tests/complex_choreography_stress_test.cpp`
* Behaviour: spawns a four-worker choreography with per-round audits and
  checksum verification across all phases. The implementation intentionally
  exercises numerous round trips and barrier usages to surface scheduling
  issues.【F:tests/complex_choreography_stress_test.cpp†L19-L139】
* Impact: the workload is significantly heavier than the regular choreography
  tests and is primarily meant as a long-running stress scenario. It is not
  intended to be part of fast CI cycles.

These tests can be re-enabled locally by uncommenting the corresponding targets
in `tests/CMakeLists.txt` and running the generated executables from the build
output.
