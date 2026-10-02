# Release baseline, 2026-10-02

**All 20 original Wisp cases ran and checked successfully.** The sweep completed
300 samples: five each for C++ Wisp and Zig Wisp on all 20 cases, Racket on the
eight program cases, and Python/Node/C on the four Gabriel cases. SBCL was not
installed and was explicitly skipped. Wren, Chibi, Ruby, and Tcl were not run.

The eight program workloads take **1.89–2.14× as long in C++ Wisp as Zig Wisp**
in this baseline. The 64-frame resumed-effect case goes the other way: C++
takes 0.632× Zig's time (1.58× faster). These are implementation measurements,
not language-wide claims or HTTP/server throughput results.

## Exact environment and policy

- C++ runtime and benchmark:
  [ec94ef5a6f69561c1712d18ba99be8919463be30](https://github.com/mbrock/nxtui/commit/ec94ef5a6f69561c1712d18ba99be8919463be30),
  clean at build and measurement. Includes the synchronous-computation GC fix
  and the concurrently landed async-filesystem work. Capability-scoped module
  loading landed during the measurement run and was merged afterward; this
  baseline remains pinned to the earlier revision, not relabeled as that merge.
- GCC/G++ 16.2.0 through the repository's locked Nix development shell;
  Meson 1.12.1, `release`, optimization `3`, `b_ndebug=true`,
  `wisp_profile=false`. C uses the same release compiler, not Clang.
- Zig reference:
  [a282b936867fcf7d5d2926ebee9afbaf3e153f5b](https://github.com/mbrock/wisp/commit/a282b936867fcf7d5d2926ebee9afbaf3e153f5b),
  cloned in `/tmp/wisp-reference`, built with its pinned Zig **0.16.0**,
  `ReleaseFast`, `semantic-profile=false`.
- CPython **3.11.6**, Node.js **v26.10.0**, Racket **9.3 [cs]**.
- Linux x86-64 E2B orb; reported CPU **Intel Xeon @ 2.60 GHz**, eight visible
  CPUs. All samples ran serially, pinned to CPU **2**, in a fixed shuffled
  order. No local builds/tests ran concurrently. VM host contention, frequency,
  and turbo were not controlled. Five samples are a baseline, not a confidence
  interval; e.g. C++ TAK ranged **91.89–98.35 ms/run**.
- C++ GC: check after `advance(run, 4096)`; collect for an explicit request or
  used bytes **≥ threshold**. Initially **1 MiB**, then **2 × live bytes + 1 MiB**.
  Used bytes include rows and payload lengths, not reserved capacity/RSS.
  Setup/warmup carry their resulting threshold into timing. Polling never
  yields. Zig instead collects every **100,000 evaluator transitions** in its
  unlimited evaluator, or on a request. The policies were not equalized.

[Raw baseline JSONL](results/2026-10-02-baseline.jsonl) contains the environment,
Meson options/compiler details, exact commands, binary hashes, all 300 samples,
and independent process wall timings. With Racket 9.3 on PATH, the measured run
was:

```sh
nix develop -c make wisp-bench-build
taskset -c 2 python3 scripts/wisp-bench --samples 5 --benchmarks all \
  --zig-repo /tmp/wisp-reference \
  --output bench/wisp/results/2026-10-02-baseline.jsonl
```

See [README](README.md) for the original Zig build command, workload counts,
warmup, provenance/license, checks, and comparison boundaries. Results below
are medians of `elapsed_ns / iterations`, not medians of process duration.

## Program measurements: milliseconds per logical run

| Program | C++ Wisp | Zig Wisp | Racket | Python | Node.js | C |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| TAK (18,12,6) | 93.157721 | 43.628374 | 0.112008 | 2.218988 | 0.205661 | 0.040922 |
| DERIV | 0.056632 | 0.029826 | 0.000242 | 0.003059 | 0.003237 | 0.000531 |
| DIVITER, 1,000 cells | 0.725017 | 0.376049 | 0.002470 | 0.054288 | 0.004660 | 0.006336 |
| DIVREC, 1,000 cells | 0.632177 | 0.335108 | 0.003178 | 0.101335 | 0.006930 | 0.007217 |
| Standard-library lists | 1.148696 | 0.558861 | 0.002198 | — | — | — |
| Backquote processing | 0.294639 | 0.148615 | 0.000405 | — | — | — |
| Continuation router hit | 0.202518 | 0.102740 | 0.001760 | — | — | — |
| Continuation router miss | 0.122577 | 0.063808 | 0.001477 | — | — | — |

“—” means no equivalent port in this run, not zero time. Python/JavaScript
division uses explicit pair nodes, not quadratic host-array slicing. Racket
and Node use their normal JIT paths; C's fixed arena does not reclaim timed
allocations. The list benchmark exercises each Lisp's own library, while
backquote ports Wisp's algorithm rather than timing a native macro expander.

**The router rows run the old guest continuation-based matcher on fixed data.**
They include capture/discard at route mismatches and nested response prompts.
They do **not** measure the new pure C++-host router, networking, request
parsing, or HTTP serving. Those workloads have different semantics and must
get different benchmark names if added later.

## Micro measurements: microseconds per iteration

| Case | C++ Wisp | Zig Wisp | C++/Zig time |
| --- | ---: | ---: | ---: |
| call-1 | 0.791 | 0.385 | 2.06× |
| call-2 | 0.933 | 0.443 | 2.10× |
| call-5 | 1.344 | 0.536 | 2.51× |
| call-16 | 3.152 | 0.980 | 3.22× |
| jet-add-2 | 1.166 | 0.592 | 1.97× |
| closure-leaf-2 | 1.346 | 0.640 | 2.10× |
| lookup-first-16 | 3.156 | 0.950 | 3.32× |
| lookup-last-16 | 3.233 | 0.977 | 3.31× |
| lookup-inner-8 | 3.904 | 2.284 | 1.71× |
| lookup-outer-8 | 3.872 | 2.296 | 1.69× |
| effect-shallow | 7.772 | 4.996 | 1.56× |
| effect-deep, 64 frames | 88.356 | 139.838 | 0.63× |

## Startup is outside the evaluator clock

For `call-1`, the C++ process median was **152.587 ms**, while its timed
25,000-iteration loop took approximately **19.8 ms**. Zig's process median was
**12.425 ms**. The difference includes parsing, setup, warmup, validation, and
teardown—not merely execution or process startup. This C++ harness interprets
the base library afresh; the production CLI and original Zig benchmark restore
embedded boot tapes. **These process values must not be presented as production
CLI cold-start measurements.** No production cold-start measurement was made.

## Semantic counters suggest where to investigate next

[Raw profiling JSONL](results/2026-10-02-profile.jsonl) is a separate single
diagnostic run at the same source revision with `wisp_profile=true`. It uses
`core/benchmark.zig`'s default counts and zero warmup, not the sweep's counts.
All 20 results passed. Its elapsed times are **not** the baseline above.

```sh
nix develop -c make wisp-bench-build \
  WISP_BENCH_BUILD_DIR=build/wisp-profile WISP_PROFILE=true
taskset -c 2 build/wisp-profile/bench/wisp-bench all \
  > bench/wisp/results/2026-10-02-profile.jsonl
```

- `call-1` visits about **13 list cells and 14 evaluator transitions per
  iteration**; `call-16` visits **163 cells and 44 transitions**, despite both
  doing about two lexical comparisons. `proceed()` rescans remaining arguments,
  making wide-call validation a concrete candidate for native CPU profiling.
  Semantic counts alone do not establish its wall-time share.
- First versus last lookup in a 16-slot frame gives about **2 versus 32 key
  comparisons/iteration**, but baseline times differ only about 2.4%. Looking
  outside eight frames raises comparisons from about **3 to 10**, without a
  clear timing penalty in these five samples. Optimizing lookup alone may not
  dominate these complete call workloads.
- Router hit and miss each record exactly **8 captures per request**. The late
  hit has seven mismatch captures plus its response capture; the miss has eight
  mismatches. This confirms the measured control mechanism is the original one.
- The deep-effect case records **one capture per iteration**, about **1,251
  transitions**, and **406 KTX allocations**, versus about **99 transitions and
  22 KTX allocations** for shallow effects. Its profiling run spent **13.1%** of
  instrumented time collecting, versus roughly **1–2%** for most other cases.
  This is a diagnostic measurement, not an estimate of uninstrumented GC time.

Verification on this implementation: `build/test/nxt-tests` passed **633 tests**
(four separately marked slow cases skipped); six selected Wisp integration
suites passed, including allocation-failure injection, host, HTTP, router,
files, and HTTP client. `meson test --suite benchmarks` passed in both release
and profiling builds: 20 checked fixtures, targeted semantic-counter assertions,
and four expected CLI rejections. The sweep also rejected an unsupported-only
selection and a profiling-enabled binary. Original copied fixtures/ports were
checked byte-for-byte against the clone. The unprofiled heap object has no
`steady_clock` reference; the profiling object does.

After merging capability-scoped loading at
[fb807461](https://github.com/mbrock/nxtui/commit/fb807461f5688e12983148842780bf96a7af105f),
verification was repeated: **636 main tests**, **seven Wisp integration suites**
(now including modules), and benchmark/counter checks in both build modes
passed. The four separately marked main slow cases remain skipped. Every
published program/micro median and ratio was checked against the raw JSONL.
