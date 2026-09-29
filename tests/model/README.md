# Spinlock witness litmus checks

These bounded C11 litmus tests exercise the atomic owner (`O`) and generation
(`G`) orderings. Use herdtools7 7.58 and its `c11_orig.cat` model:

```sh
herd7 -cat /path/to/herdtools7/herd/c11_orig.cat tests/model/acquire_only.litmus
herd7 -speedcheck true -cat /path/to/herdtools7/herd/c11_orig.cat tests/model/aba_withheld.litmus
```

The expected result for the forbidden conditions in `acquire_only`,
`takeover_bump`, `aba_withheld`, `release_reacquire`, `stale_winner`, and
`bump_marks` is `No`. The paired `*_calibration` cases must report `Ok` for
the intended handoff, preventing a branch that never executes from passing
unnoticed. These are expected outcomes, not recorded verification results.
The introducing commit (`37a2b54c`) records that no new model run was performed;
its validation does not establish results for these checked-in inputs.

`acquire_only` tests visibility through a zero release and an acquire-only
winning CAS. `takeover_bump` tests visibility of a +2 bump through the
takeover CAS. `aba_withheld` tests two takeovers returning to the original
owner bits with both post-CAS marks withheld. `release_reacquire` tests the
same owner bits after zero and a new acquisition. `stale_winner` tests a
contender whose zero probe predates another complete hold. `bump_marks`
checks that a losing taker's +2 cannot be overwritten by the holder's RMW
parity marks.

The two same-owner tests and the stale-contender test use release/acquire
handshakes to select the schedules under examination. These tests are bounded
checks of those schedules. They do not establish the generation no-wrap
premise, OS thread-CPU-clock behavior, or recovery exclusion under a repeated
PID/token word.
