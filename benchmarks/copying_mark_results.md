# Copying-mark broadcast comparison, 2026-09-27

The public broadcast-to-handler workload improves consistently for small
messages with 4, 8 and 16 readers. All 108 samples below completed equal work:
210,161,856 verified deliveries, with zero loss, order/payload errors or
reported evictions. The medium-frame throughput tradeoffs and every repeated
sample remain in [the CSV](copying_mark_samples.csv).

## Measurement

Baseline is `9d19850a`; candidate is this copying-mark implementation. Both
executables used the same benchmark source, Release `/O2 /Ob2 /DNDEBUG`,
production hooks disabled, VS2026 MSVC v145 14.51.36231 x64, `/Z7`, CMake 4.4.2
and FASTBuild 1.20 distributed through XENIA. Both builds reported `FBuild: OK:`.
The measurements ran on Windows host ANDREAS, AMD Ryzen 7 7840U (8 cores,
16 logical processors), approximately 63.2 GiB RAM.

Each sample broadcasts 1,000,000 64-byte, 100,000 4096-byte, or 4096 261632-byte
application payloads. Warmup is 5% of the measured count. Public processing
fences drain identical bursts of 910, 113 or 2 messages respectively, bounded
by a quarter ring with 512 bytes of framing headroom per message. The largest
payload fits below the 262144-byte frame limit. The measured time includes
all burst drains and the final processing fence, so every recipient's handler
has completed. This is a paced end-to-end comparison, not a saturation limit.
Unequal-delivery runs cannot establish a speedup and are excluded from the
comparison. No paced sample saturated or reported eviction.

The full matrix has three adjacent pairs per workload, alternating revision
order. Confirmations repeat the same binary, message counts and pacing;
the first confirmation starts with the candidate and the additional tail
confirmation starts with the baseline. Each sample has a 30-second watchdog.
Throughput and p99 columns are medians per revision; paired changes are the
median and range of candidate/baseline ratios for corresponding runs. A ratio
of the two throughput medians can differ from the median paired ratio. These
ranges show observed variability, not confidence intervals.

Host CPU was 1.89% to 46.02% across the sample lifetimes; this was a shared
workstation with varying background activity. Host utilization includes
setup and report generation. Writer CPU covers the measured interval;
aggregate reader CPU covers each reader's first through last measured handler.
Native Windows CPU accounting was coarse for short paced bursts, often zero
or increments of 0.015625 seconds. The CSV retains those measurements; tiny
CPU differences should not be interpreted as precise savings.

The CSV records delivered messages and bytes per second, writer and aggregate
reader CPU, handler latency p50/p95/p99/maximum, broadcast-call p99/maximum,
processing-drain p99/maximum, host load, and all delivery/error/eviction counts.
Broadcast-call time includes serialization, reservation waits and API overhead;
it bounds a writer wait inside that call but does not isolate the octile wait.
Drain time measures a whole public processing barrier. Setup, result-file
writing and shutdown are outside the throughput interval. See the
[usage instructions](README.md) to reproduce the default matrix.

## Full matrix

| Readers | Payload bytes | Baseline deliveries/s | Candidate deliveries/s | Paired change: median [min, max] | Handler p99, baseline/candidate (ms) |
| ---: | ---: | ---: | ---: | --- | ---: |
| 1 | 64 | 115,566 | 113,925 | -0.7% [-2.1%, +5.8%] | 1.304 / 1.129 |
| 1 | 4096 | 16,087 | 15,873 | -1.3% [-10.5%, +0.9%] | 1.504 / 1.501 |
| 1 | 261632 | 279 | 279 | +0.3% [-0.6%, +0.4%] | 1.756 / 1.834 |
| 2 | 64 | 180,949 | 195,176 | +7.3% [-2.7%, +7.9%] | 3.778 / 2.414 |
| 2 | 4096 | 26,507 | 16,465 | -25.1% [-38.0%, -3.3%] | 1.521 / 6.668 |
| 2 | 261632 | 516 | 552 | +7.1% [+4.6%, +12.6%] | 1.836 / 1.762 |
| 4 | 64 | 400,319 | 428,094 | +7.7% [+3.7%, +9.8%] | 1.392 / 1.062 |
| 4 | 4096 | 53,475 | 53,133 | -8.8% [-17.0%, -0.6%] | 1.365 / 1.113 |
| 4 | 261632 | 969 | 965 | -0.4% [-5.0%, +7.4%] | 1.773 / 1.782 |
| 8 | 64 | 715,887 | 805,889 | +11.6% [+10.9%, +26.7%] | 1.826 / 1.351 |
| 8 | 4096 | 97,927 | 97,311 | -1.4% [-2.6%, -0.6%] | 1.536 / 1.499 |
| 8 | 261632 | 1,734 | 1,757 | +3.3% [-1.6%, +3.9%] | 1.841 / 1.775 |
| 16 | 64 | 1,193,911 | 1,315,967 | +11.0% [+8.5%, +14.6%] | 3.166 / 1.538 |
| 16 | 4096 | 163,181 | 156,137 | -4.3% [-5.3%, -1.2%] | 1.796 / 1.683 |
| 16 | 261632 | 2,415 | 2,647 | -0.1% [-0.6%, +12.6%] | 5.299 / 3.195 |

## Bounded confirmations

| Readers | Payload bytes | Baseline deliveries/s | Candidate deliveries/s | Paired change: median [min, max] | Handler p99, baseline/candidate (ms) |
| ---: | ---: | ---: | ---: | --- | ---: |
| 2 | 4096 | 26,129 | 26,216 | -3.0% [-3.2%, +3.4%] | 1.560 / 1.737 |
| 4 | 4096 | 50,653 | 50,336 | -0.6% [-2.0%, +1.2%] | 1.581 / 1.533 |

## Additional two-reader tail confirmation

| Readers | Payload bytes | Baseline deliveries/s | Candidate deliveries/s | Paired change: median [min, max] | Handler p99, baseline/candidate (ms) |
| ---: | ---: | ---: | ---: | --- | ---: |
| 2 | 4096 | 29,084 | 29,599 | +1.8% [+0.6%, +6.5%] | 1.486 / 1.488 |

## Interpretation

Small-message paired throughput improves at 4, 8 and 16 readers in every
matrix pair: median +7.7%, +11.6% and +11.0%. At 16 readers, median throughput
is 1.194 to 1.316 million delivered messages/s (76.4 to 84.2 MB/s of application
payload), and handler p99 falls from 3.166 to 1.538 ms. Aggregate reader CPU
medians decrease from 0.594 to 0.047 seconds at 8 readers and 1.516 to 0.313
seconds at 16 readers, subject to the accounting granularity above. Worst
broadcast-call observations across those three pairs decrease from 1.378 to
0.365 ms and from 10.179 to 1.206 ms respectively. These results are consistent
with removing serialization between readers copying small frames; the
benchmark does not independently attribute every saved cycle to that lock.

Single-reader paired throughput changes are -0.7%, -1.3% and +0.3% for the
three sizes. Handler p99 changes from 1.304 to 1.129 ms, 1.504 to 1.501 ms and
1.756 to 1.834 ms. Broadcast p99 medians are 1.8 to 1.6 microseconds, 3.0 to
3.2 microseconds and 76.2 to 82.2 microseconds. Isolated maxima vary: near-limit
worst broadcast time is 0.391 to 0.612 ms, while worst handler latency falls
from 2.888 to 2.212 ms. There is no repeated material single-reader throughput
or handler-tail regression in these samples.

The original two-reader 4096-byte cohort has a large regression and is
retained. Its first two candidate samples coincided with 40.5% and 46.0% host
CPU versus paired baseline 13.3% and 7.6%; candidate drain p99 was 67.1 and
48.9 ms versus baseline 21.2 and 11.9 ms. The quietest original pair was
-3.3%, with drain p99 11.48 versus 11.60 ms. These observations associate the
large slowdown with host/coordination delays, but do not prove that all
outliers came from external load.

The first bounded confirmation reduces that cohort's paired slowdown to
-3.0%, but still shows a 177-microsecond increase in median handler p99. That
residual was investigated with three additional unchanged-workload pairs:
paired throughput changes are +0.6%, +1.8% and +6.5%; handler p99 differences
are +14.7, +11.6 and -7.7 microseconds. Median handler p99 is then 1.486 to
1.488 ms, and broadcast p99 is 3.1 to 2.9 microseconds. Host utilization for
those samples is 5.5% to 8.8%. Thus neither the large regression nor the
177-microsecond residual repeats in the added bounded measurement. The
supported conclusion is variation with load and scheduling, rather than a
repeatable protocol tail regression; the retained samples do not establish
that every individual delay is external.

Four-reader 4096-byte confirmation changes paired throughput by -0.6%
(range -2.0% to +1.2%) and handler p99 by 1.581 to 1.533 ms. The original
16-reader 4096-byte throughput tradeoff remains: paired median -4.3%, with
all three pairs between -5.3% and -1.2%, although handler p99 improves from
1.796 to 1.683 ms. Medium frames therefore do not demonstrate a general
throughput gain. Millisecond processing-drain tails versus microsecond
broadcast calls suggest that coordination costs limit this paced workload;
these observations do not measure an exact fraction of time spent draining.

The measured acceptance evidence is repeated many-reader small-frame
improvement, single-reader throughput parity, and no repeatable material
handler-tail regression after bounded investigation. It does not claim a
speedup for every payload or eliminate shared-host uncertainty.

## Input identity

Benchmark source SHA-256:
`50314780B7D2282AD194C18FC86F7404CB3267890332632F1867CAF5BD0DBAA4`.
The source bytes used for baseline and candidate builds were identical.

Immutable measured executable SHA-256 values:

- Baseline: `966BB06DAF2CB668D9F31A79F960DB4FCF0DBA2C81071C2B1CE359C5F9F71847`.
- Candidate: `80E2D09F4C8C0895225E8A970B92867095EE6E50E225527411C465B48ED6C4FE`.

The CSV uses `matrix`, `confirmation` and `tail_confirmation` phase labels;
all rows are retained, including the background-load outliers. Its Unix
start timestamps and host utilization allow the paired ordering and load
to be audited. The full matrix and confirmations use the same two immutable
executables.
