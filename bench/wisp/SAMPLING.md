# Wisp evaluation: CPU sampling, 2026-10-04

Profiled [3b543ec](https://github.com/mbrock/nxtui/commit/3b543eceef2d9010372c13c17217add7bc0bd648),
without changing the evaluator or benchmark harness. **Source call processing,
lowered operand decoding, and binding are the main ordinary-workload targets.**
Deep effects are different: long runs retain substantial heap and spend much
more time collecting it. Investigate that retention before optimizing capture.

## Method and limits

- Locked Nix Clang 23.1.0, Meson release (`-O3`, `b_ndebug=true`) with
  `debug=true`; semantic counters disabled. Normal toolchain hardening remains
  enabled. No frame-pointer or evaluator changes.
- Linux x86-64 E2B orb, Intel Xeon @ 2.60 GHz, pinned to CPU 2. Hardware
  `cycles` and `instructions` events report **not supported**. These results
  cannot establish IPC, branch-miss rates, or cache-miss causes.
- `perf` 6.1.187, `cpu-clock:u`, 997 Hz, DWARF call chains with 8,192-byte
  stack captures. Seven workloads in source and lowered-library modes, one
  long profile each, serially shuffled with seed 20261004. Counts were
  calibrated for roughly 12 seconds of evaluator execution; actual evaluator
  clocks were 11.55–15.23 seconds. No concurrent builds or tests during capture.
- Zero warmup, recording delayed by 2,000 ms to skip setup and the first part
  of evaluation. This is **steady-state sampling**, not an exact recording of
  the benchmark clock. Compiler/lowering performance is not measured.
- All 14 results passed independent fixture validation. There were **153,691
  process samples, zero reported lost samples**. Call-stack analysis narrowed
  these to **149,568 samples** between the first and last identifiable samples
  in the timed evaluator loop. No loader frames appeared in those windows.
- The clock loop was identified from this binary's disassembly: `main+0xbe0`
  through `main+0xd40`, with `advance` returning at `+0xc8d` and timed
  collection at `+0xbe7`. These offsets are **binary-specific**, not an API.
  Filtering each recording to that timestamp window removes post-validation
  collection, notably thousands of samples in deep effects. Edges are
  approximate to sampling resolution. About 3–5.5% of process samples lack a
  resolved `main` frame; samples inside the window are retained, not discarded.
- Self percentages below combine identically named function/PLT rows. GC and
  binding percentages are inclusive stack-presence counts, which can miss
  ancestors in incomplete stacks. They are not an additive time breakdown.
  Instruction annotations can skid; do not interpret one hot instruction as
  proof of a microarchitectural stall.
- This is a hotspot survey, **not a repeated timing comparison or confidence
  interval**. Iteration counts differ between modes, particularly affecting
  effect retention. Host contention and frequency were not controlled.

[JSONL](results/2026-10-04-sampling.jsonl) preserves binary/library hashes,
Meson options, CPU, commands, calibration, checked results, sample windows,
full symbol counts, and the retention follow-up.
[Text reports](results/2026-10-04-sampling.txt) preserve both whole-recording
and windowed self reports, scan callers, and operand-decoding annotation.
The large `perf.data` recordings remain under `build/wisp-sampling/profiles/`
in the profiling orb; they are not committed.

## Ordinary evaluation

Percentages are from the sample-bounded evaluation windows, not setup.

| Workload | Source: leading self costs | Lowered library: leading self costs | GC inclusive, source / lowered |
| --- | --- | --- | ---: |
| TAK | `proceed` 16.0%, `application` 10.8%, `lookup` 10.2% | `immediate` 12.1%, `once` 11.3%, `lowered` 9.4% | 1.8% / 6.1% |
| 16-argument call | `scan_count` 23.8%, `proceed` 18.8%, `execute` 9.4% | `immediate` 25.6%, `bind` 11.5% | 1.6% / 12.0% |
| DERIV | `proceed` 15.1%, `application` 14.2%, `execute` 9.9% | `immediate` 12.3%, `lowered` 11.6%, `once` 10.1% | 1.7% / 5.4% |
| Recursive division | `proceed` 15.3%, `application` 13.6%, `execute` 10.5% | `immediate` 11.6%, `once` 9.8%, `proceed_lowered` 9.1% | 3.1% / 7.5% |
| Standard-library lists | `proceed` 15.7%, `application` 11.7%, `execute` 9.7% | `immediate` 13.6%, `lowered` 10.0%, `once` 9.4% | 1.8% / 6.4% |
| Guest router hit | `proceed` 13.5%, `application` 13.1%, `execute` 9.1% | `lowered` 15.0%, `immediate` 11.2%, `proceed_lowered` 9.9% | 1.9% / 6.5% |

The router is the old guest continuation-based matcher, not networking or the
pure host router. Its identifiable `send`/`send_from` stacks account for
about 0.6% of source-window samples. Some continuation work is inlined into
other symbols, so this is not a measurement of all continuation overhead;
ordinary evaluation is nevertheless the much stronger visible signal here.

### Source calls repeatedly validate remaining argument spines

The source `call-16` profile attributes 23.0% of all process samples to the
out-of-line `scan_count` body, almost entirely called from `proceed`; its
additional PLT row brings the function total to 23.8%.
In [the ordinary-call branch of `proceed`](../../src/wisp/eval.cpp), each
completed argument invokes `scan_count(arg)` on the remaining list. For a
16-argument call, that visits 15 + 14 + … + 1 = **120 remaining cells**, in
addition to the initial application scan and parameter processing.

This identifies a concrete optimization experiment, not permission to remove
validation: source conses are mutable, arguments can execute arbitrary guest
code, and malformed/cyclic lists must still be detected. A cached count would
need a semantics-preserving invalidation or validation strategy.

### Lowering trades source traversal for decoding and binding

`immediate` is not arithmetic: it recognizes a compact-code record, decodes
its opcode, then reads a constant, lexical slot, or function cell.
The `call-16` annotation places substantial samples in tag/opcode decoding
and its prologue, not in an expensive guest addition. The 25.6% function
total includes about 2.0% in the matching PLT entry; the annotated body alone
is 23.6%.

`bind` and descendants occupy **14–26%** of lowered ordinary-workload
windows: 20.7% in TAK and 25.6% in `call-16`. Lowered closures still use the
same parameter-list binder and allocate a name/value scope vector plus an
environment cons. Lowered code has therefore not eliminated call setup.
Typed parameter metadata or a measured fixed-arity fast path is a plausible
next experiment, subject to optional/rest arguments, mutable parameters,
reflection, and lexical-store semantics.

Higher GC percentages in lowered mode do not by themselves prove more
allocation or slower collection. The lowered fixture retains more library
code, uses different collection thresholds, and performs less source work;
percentages have different denominators.

## Deep effects: retained heap changes the workload

The long runs retain **1,232.8 MiB** in source mode (290,620 effects) and
**1,504.4 MiB** lowered (356,845 effects), after final collection while the
completed evaluator remains rooted. The same measurements for the other
six cases stay around 0.05–0.26 MiB.

Within the evaluation windows, identifiable GC stacks account for **21.8% /
28.5%** of source/lowered samples. The whole-process profile overstates this
because its final untimed collection is substantial: 1,703 / 2,209 samples
have resolved `main` frames outside the timed loop, with additional
incomplete stacks outside the bounded window.

An unprofiled follow-up at 1, 10, 100, 1,000, and 10,000 effects reproduced
exact linear retained-heap growth. For counts 10 through 10,000, retained
bytes above the pre-run live heap are:

| Case | Source | Lowered library |
| --- | ---: | ---: |
| Shallow effect | 352 × count + 28 | 324 × count + 36 |
| 64-deep effect | 4,448 × count + 28 | 4,420 × count + 36 |

The depth difference is exactly **4,096 bytes per effect** in both modes.
At 10,000 deep effects, that is roughly 42 MiB retained, not just temporary
allocation before GC. All 20 follow-up results checked successfully.

This establishes retention in this rooted benchmark, **not its cause or a
general application leak**. `call-with-effect-handler` creates closures
around captured continuations when reinstalling its prompt; tracing their
reachability and the completed run's environment is the next diagnostic.
Do not compare unequal-length deep-effect runs as a clean dispatch test, or
assume `send` itself is the dominant expense.

## Reproduce and continue

On Debian the profiler was installed with `sudo apt-get install linux-perf`.
Configure the normal benchmark build, then enable debug information without
changing release optimization:

```sh
nix develop -c make wisp-bench-build WISP_BENCH_BUILD_DIR=build/wisp-sampling
nix develop -c meson configure build/wisp-sampling -Ddebug=true
nix develop -c meson compile -C build/wisp-sampling wisp-bench
mkdir -p build/wisp-sampling/profiles
perf record -e cpu-clock:u -F 997 --call-graph dwarf,8192 --delay=2000 \
  -o build/wisp-sampling/profiles/source-call-16.data -- \
  taskset -c 2 build/wisp-sampling/bench/wisp-bench call-16 11352886 0
perf report -i build/wisp-sampling/profiles/source-call-16.data --stdio \
  --no-children --no-inline --sort dso,symbol --call-graph none -n
perf script -i build/wisp-sampling/profiles/source-call-16.data \
  --no-inline -F time,ip,sym,symoff,dso
```

All 14 exact capture commands are in the JSONL `sample` records. Iteration
counts were round(12,000,000,000 / calibration `ns_per_iteration`), with zero
warmup. For each binary, inspect the timed-loop call sites anew; do not reuse
the archived offsets after rebuilding different code. Group `perf script`
samples at blank lines, find the first/last timestamps whose `main` offset
is in that loop, then use `perf report --time START,END`. Archived
`stack-analysis` records contain the windows used here. Inclusive counts
test each stack once for the corresponding function substring; self counts
use its first frame, merging identical names.

**Suggested order:** trace effect retention first; measure a lowered
fixed-arity binder/operand-decoding experiment next; consider source argument
scanning only with an explicit mutable-list correctness argument. Use paired,
unprofiled repeated timings to validate any proposed speedup. No evaluator
optimization was made in this profiling pass.
