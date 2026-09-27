# Public broadcast benchmark

Configure Release with `-DSINTRA_BUILD_BENCHMARKS=ON`. The
`sintra_broadcast_benchmark` target links the public header-only library with
production hooks disabled. Build both revisions with identical toolchain,
optimization, debug-information and instrumentation settings.

The coordinator calls `sintra::world() << message`; each leaf process handles
the complete message through a public activated slot. Explicit
`processing_fence_t` barriers close each burst and the measured interval after
handler delivery. Every recipient verifies
message order, payload size and a payload sentinel. Samples with incomplete
delivery are rejected rather than compared as throughput improvements.

```text
python benchmarks/run_broadcast_benchmark.py --baseline <base-exe> --candidate <new-exe> --output samples.jsonl
```

The default matrix uses 1, 2, 4, 8 and 16 reader processes; 64-byte, 4096-byte
and 261632-byte application payloads; warmup; and three paired samples with
alternating revision order. The largest payload stays within one frame below
the 262144-byte frame limit. Each burst fits within a quarter ring, allowing
512 bytes of framing headroom per application message. Thus both revisions
drain the same bounded work before wrapping, and the measured end-to-end time
includes the processing-fence backpressure. Warmup follows the same pacing.
The executable accepts `--burst 0` for unpaced saturation diagnostics.
Set `--readers` to the practical process counts
for the host. Each sample has a 30-second watchdog, independently of the
library's progress policy.

JSON records contain verified delivered messages and bytes per second,
writer and aggregate reader CPU seconds, handler latency p50/p95/p99/maximum,
broadcast-call p99/maximum, loss/order errors and eviction log reports. The
broadcast-call duration includes serialization, reservation waits and public
API overhead; it is an upper-bound observation of writer waits, not an
isolated octile-wait timer. Separate drain p99/maximum fields measure processing
barrier duration. Windows host CPU utilization covers the entire
sample process lifetime, including setup; process CPU and throughput cover
the measured interval. Reader CPU starts at its first measured handler and
ends at its last handler.

On Windows, the native process CPU accounting can be coarse for short bursts:
the recorded values may be zero or advance in 15.625 ms steps. Retain those
values, but do not infer fine-grained CPU savings from them.

Retain each record to show variability. Compare only complete paired samples
with equal payload sizes, process counts and message counts. Report incomplete
or eviction-heavy runs separately as saturation, and explain material
single-reader or tail regressions rather than changing the workload to hide
them.

The [copying-mark comparison](copying_mark_results.md) records one complete
Windows matrix and its bounded confirmation samples. The accompanying CSV
keeps every sample's metrics, including the slower cohorts and host load.
