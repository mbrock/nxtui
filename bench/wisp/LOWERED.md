# Compact lowered code: first measurements

> Record execution (the "Record" columns and the `prepared` modes) was
> retired after these measurements; see RFC 0021's native profile. The raw
> data remain as recorded.

Measured at [626484a](https://github.com/mbrock/nxtui/commit/626484af31129089d838fd19ee992faec193ea97),
2026-10-04. This is RFC 0021's compact-node implementation, not a flat
instruction-stream VM. All **500 timing samples** and **100 diagnostic
records** passed their independent result checks.

## Method

- Locked Nix `.#clang` shell, Clang **23.1.0**, Meson **1.12.1**, release
  optimization, `b_ndebug=true`. Timings have `wisp_profile=false`; diagnostics
  use a separate release build with `wisp_profile=true`.
- Linux x86-64 E2B orb, Intel Xeon @ 2.60 GHz, eight visible CPUs. Samples ran
  serially, pinned to CPU **2**, five per case/mode in a fixed shuffled order,
  with no concurrent builds or tests. Host contention, turbo, and frequency
  were not controlled. Five samples are not a confidence interval: source TAK
  ranged **55.03–71.84 ms**, lowered-library TAK **55.35–65.93 ms**.
- The same binary runs all five modes. Source excludes the compiler;
  prepared/lowered load it. The `-library` modes also transform the base
  library and compiler. Parsing, transformation, warmup, and result checking
  are outside the evaluator clock; process times are not startup measurements.
- Each timed run begins with an untimed collection and threshold reset to
  **2 × live bytes + 1 MiB**. Poll every 4,096 transitions. Heap sizes count
  rows and payload lengths, not reserved capacity or RSS. This differs from
  the setup-garbage/threshold treatment in the 2026-10-02 baseline; compare
  the contemporaneous source samples here, not that historical baseline.

[Timing JSONL](results/2026-10-04-lowered.jsonl) retains commands, binary hash,
compiler/build metadata, affinity, and every sample. [Diagnostic JSONL](results/2026-10-04-lowered-profile.jsonl)
retains the corresponding profiling-build metadata and commands. The timing
sweep uses the existing cross-language workload counts and warmup. Diagnostics
use default benchmark counts and zero warmup; their clocks are **not** the
uninstrumented timing results.

```sh
nix develop .#clang -c meson setup build/wisp-lowered --buildtype=release \
  -Db_ndebug=true -Dbenchmarks=true -Dtests=true -Ddemo=false -Ddev=false \
  -Dllm_tool=false -Dwisp_tool=true -Ddefault_wand=epoll \
  -Dc_link_args=-fuse-ld=mold -Dcpp_link_args=-fuse-ld=mold
nix develop .#clang -c meson compile -C build/wisp-lowered
taskset -c 2 python3 scripts/wisp-bench --build-dir build/wisp-lowered \
  --runtimes cpp --samples 5 --benchmarks all \
  --wisp-modes source,prepared,prepared-library,lowered,lowered-library \
  --output bench/wisp/results/2026-10-04-lowered.jsonl
```

For diagnostics, configure a separate `build/wisp-lowered-profile` directory
with the same compiler/options and `-Dwisp_profile=true` (tests and CLI may be
disabled). After building, run `wisp-bench all` and each of
`--prepared all`, `--prepared-library all`, `--lowered all`, and
`--lowered-library all`, serially under `taskset -c 2`. Every saved diagnostic
includes its exact command.

## Program medians: milliseconds per logical run

| Case | Source | Record | Record library | Lowered | Lowered library |
| --- | ---: | ---: | ---: | ---: | ---: |
| TAK | 56.207 | 58.525 | 64.517 | 53.738 | 60.371 |
| DERIV | 0.035658 | 0.042069 | 0.041950 | 0.039242 | 0.039088 |
| DIVITER | 0.470988 | 0.647317 | 0.584189 | 0.579789 | 0.519825 |
| DIVREC | 0.416122 | 0.597338 | 0.540821 | 0.521611 | 0.453874 |
| Standard-library lists | 0.723380 | 0.757507 | 0.783412 | 0.722738 | 0.684264 |
| Backquote | 0.173596 | 0.190190 | 0.228398 | 0.190315 | 0.207508 |
| Router hit | 0.121855 | 0.124961 | 0.114551 | 0.125521 | 0.108876 |
| Router miss | 0.080585 | 0.074978 | 0.071161 | 0.073191 | 0.067414 |

The router is the **old guest continuation-based matcher**, not the pure host
router, HTTP serving, or networking.

## Micro medians: microseconds per iteration

| Case | Source | Record | Record library | Lowered | Lowered library |
| --- | ---: | ---: | ---: | ---: | ---: |
| call-1 | 0.508 | 0.473 | 0.483 | 0.392 | 0.411 |
| call-2 | 0.585 | 0.529 | 0.532 | 0.444 | 0.470 |
| call-5 | 0.730 | 0.619 | 0.660 | 0.538 | 0.548 |
| call-16 | 1.421 | 0.936 | 0.981 | 0.808 | 0.870 |
| jet-add-2 | 0.757 | 0.620 | 0.643 | 0.551 | 0.541 |
| closure-leaf-2 | 0.801 | 0.744 | 0.762 | 0.669 | 0.677 |
| lookup-first-16 | 1.478 | 0.953 | 0.987 | 0.829 | 0.889 |
| lookup-last-16 | 1.461 | 0.909 | 0.940 | 0.870 | 0.885 |
| lookup-inner-8 | 2.355 | 2.029 | 2.244 | 1.855 | 1.911 |
| lookup-outer-8 | 2.318 | 2.210 | 2.241 | 1.884 | 1.947 |
| effect-shallow | 4.824 | 4.018 | 3.749 | 3.907 | 3.306 |
| effect-deep | 68.491 | 62.871 | 59.962 | 56.775 | 59.262 |

## Retention and execution diagnostics

The `call-1` fixture machine's live heap before execution, with no warmup:

| Mode | Live KiB |
| --- | ---: |
| Source | 56.049 |
| Record | 118.644 |
| Record library | 327.386 |
| Lowered | 114.565 |
| Lowered library | 255.862 |

These are **whole fixture machines**, not isolated library/code sizes. The
fully lowered machine retains **21.8% less** than the fully prepared machine,
but remains larger than source, which does not load the compiler. Heap sizes
for every workload/mode are retained in both raw files.

TAK diagnostics, per logical run:

| Mode | Transitions | Frame pushes | Word-pool words | Copied progress words | Instrumented GC ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Source | 1,463,015 | 365,754 | 1,097,649 | 391 | 2.182 |
| Record | 826,921 | 254,437 | 763,766 | 460 | 2.671 |
| Record library | 826,921 | 254,437 | 763,658 | 352 | 5.684 |
| Lowered | 826,921 | 254,437 | 763,766 | 460 | 2.453 |
| Lowered library | 826,921 | 254,437 | 763,710 | 404 | 4.636 |

All 40 matched record/lowered workload pairs preserve transition and frame
push counts exactly. Payload-word differences in library mode equal the
differences in copied-progress words exactly: collection freezes surviving
frames, so changing live heap and collection boundaries changes subsequent
copy-on-write work even without a guest continuation capture.

Per iteration in fully lowered mode, shallow effects push **11.001 frames**,
allocate **63.02 words**, and copy **1 word** of progress; deep effects push
**203.001**, allocate **575.02**, and copy **193**. Router hit/miss push
**399.99/263.99**, allocate **1,169/731** words, and copy **0/0** in this
diagnostic run. This counter measures actual progress cloning, not the size
of every captured segment or hypothetical activation-sized copies.

GC improves on some workloads but not uniformly. The diagnostic deep-effect
run collects four times fully lowered versus three times prepared-library and spends
**17.1 µs versus 9.8 µs per iteration** collecting. This is a single
instrumented observation, not a stable uninstrumented GC percentage.

## What this establishes, and what comes next

Compact code is a better execution representation than analysis records:
fully lowered medians beat fully prepared medians in all 20 cases, and it
releases the binding/owner graph while preserving inspectable control.
Against source, wide calls improve **1.63×**, shallow effects **1.46×**, and
the routers **1.12×/1.20×**. But this does **not** yet make whole-library
lowering generally faster: TAK is **7.4% slower**, DERIV/division roughly
**9–10% slower**, and backquote **19.5% slower** than source.

The RFC's hoped-for equality of average source/lowered transition cost is
not established. Dividing uninstrumented TAK time by diagnostic transition
counts gives about **38 ns/source transition** versus **65 ns/lowered** and
**73 ns/lowered-library**. These are different mixes of work (immediate
operands fuse several source steps); they do not isolate opcode-check cost.

Keep compact nodes and per-operation frames for now. The next experiment
should native-profile the remaining call/binding/frame-allocation paths,
including effects, before designing flat code or activation-wide temporaries.
TAK still pushes 254,437 frames and allocates about 764,000 word-pool words;
lowering intentionally did not change that. Fewer allocations are a concrete
candidate, not a demonstrated dominant CPU bottleneck. Neither an immutable
code type nor dropping source snapshots is justified by these counters alone.
Keep record execution as the executable semantic reference.

## Verification

- Release `meson test -C build/wisp-lowered --print-errorlogs`: **16 passed,
  four expected CLI rejections, zero failures**. This includes 846 everyday
  tests, all 10 slow tests, allocation-failure injection, seven host integration
  suites, and 20 checked fixtures in each of five modes.
- Profiling build benchmark suite: six passed, four expected rejections,
  zero failures, including the copied-progress counter assertions.
- ASan + UBSan execution-focused selection: **137 passed**, five slow cases
  skipped. The broader sanitizer attempt hit the existing one-second deadlines
  in compiler cold boot and the aggregate corpus IR-checker test; those checks
  pass in release, but their sanitizer coverage is not claimed. No sanitizer
  diagnostic appeared in the passing selection.
- A lowered CLI function was checkpointed at a timer await; restoring the same
  tape twice in separate processes printed **42** each time. Native tests
  separately restore two machines and resume each twice with collection after
  every transition, validating independent progress and shared lexical mutation.
- The lowered corpus runs with IR descriptor names disabled. A native retention
  test drops its analysis graph, collects away both binding objects, then runs
  and inspects the lowered closure without recognizable IR descriptors.
