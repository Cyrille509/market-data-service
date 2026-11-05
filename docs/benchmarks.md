# Benchmark methodology and results

`replay_bench` measures deterministic, synthetic replay through the same
parser, sequencing state machine, and order-book implementation used by the
live service. It reports latency and throughput from separate passes so that
per-message clock reads do not distort the throughput result.

## Workload

The default run uses a fixed-seed stream with:

- 20,000 untimed warm-up messages
- 200,000 measured raw JSON depth messages
- 5 bid and 5 ask updates per message (2,000,000 measured level updates)
- 500 initial levels on each side
- 180,046 adds, 1,619,956 modifications, 180,047 successful deletes, and
  19,951 absent-level delete no-ops

```powershell
build\replay_bench
# replay_bench [measured_frames] [levels_per_side] [warmup_frames]
```

## Measurement boundaries

The executable makes three independent passes over the same workload:

1. **Order-book mutation latency:** one decoded `Level` passed to
   `OrderBook::set_bid` or `set_ask`.
2. **Raw-message-to-book service latency:** starts immediately before
   `parse_depth_frame(raw_json)` and ends after `SyncEngine::on_frame`
   returns. It includes JSON and fixed-point decoding, sequence validation,
   ten book mutations, crossed-book validation, and counters.
3. **Full-path throughput:** the same raw JSON path, timed once around the
   complete loop without per-message clock reads.

Input generation, initial snapshot synchronization, warm-up, sorting, and
output are outside the per-message timed regions. Samples use
`std::chrono::steady_clock`; the measured back-to-back clock cost is reported
but not subtracted.

Both end-to-end passes must finish with zero gaps, the expected counters and
sequence ID, and a non-crossed book. All three passes must reach the same full
book fingerprint; the default workload also checks the golden value
`5e2ed2b33c4aa494`.

## Recorded environment

Measured August 20, 2026:

| Item | Configuration |
|---|---|
| CPU | Snapdragon X X1E80100, 12 cores, up to 3.4 GHz |
| OS | Windows 11 ARM64 10.0.26200 |
| Binary | x64 running under emulation |
| Compiler | clang 22.1.8, C++17, `-O3 -DNDEBUG` |
| Execution | Single thread; five fresh processes, each running all three passes |
| Scheduling | No CPU affinity, real-time scheduling, or outlier filtering |

The recorded x64 binary was configured and built with:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  "-DCMAKE_C_FLAGS=--target=x86_64-pc-windows-msvc" `
  "-DCMAKE_CXX_FLAGS=--target=x86_64-pc-windows-msvc" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Results

Raw-message-to-book service latency, with 200,000 samples per run:

| Run | p50 | p95 | p99 | p99.9 | Mean | Max |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1.4 us | 1.7 us | 1.9 us | 4.2 us | 1.504 us | 2.699 ms |
| 2 | 1.4 us | 4.2 us | 4.8 us | 15.2 us | 2.199 us | 7.733 ms |
| 3 | 1.4 us | 1.8 us | 2.1 us | 4.7 us | 1.548 us | 2.606 ms |
| 4 | 1.4 us | 1.7 us | 1.9 us | 3.7 us | 1.504 us | 3.238 ms |
| 5 | 1.4 us | 3.7 us | 4.4 us | 17.3 us | 2.252 us | 45.262 ms |

Uninstrumented full-path throughput:

| Run | Elapsed | Frames/sec | Level updates/sec |
|---:|---:|---:|---:|
| 1 | 0.307989 s | 649,374 | 6,493,736 |
| 2 | 0.334813 s | 597,348 | 5,973,484 |
| 3 | 0.323243 s | 618,729 | 6,187,292 |
| 4 | 0.330140 s | 605,803 | 6,058,030 |
| 5 | 0.315546 s | 633,823 | 6,338,226 |

The five-run p99 range was 1.9-4.8 us. Throughput ranged from 5.97M to
6.49M applied price-level updates/sec. The direct order-book microbenchmark
measured 300-500 ns p99, but its 100 ns clock granularity is material, so it
is treated as a secondary result.

## Interpretation

- This is warmed, in-memory processing service time, not exchange-to-process
  or network latency.
- Latency and throughput are separate measurements of the same workload.
- All ten updates in a message are applied serially.
- Windows scheduling and x64 emulation produce visible tail outliers.
- Results are observations on the recorded system, not a hardware-independent
  performance guarantee.
