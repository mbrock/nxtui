# Building, testing, and tooling {#building}

This repo builds with Meson 1.3+ and does not require Nix. Building requires a
C++23 compiler with `#embed` support (GCC 15+ or Clang 19+). `#embed` is
standard in C23/C++26 and supported as an extension in C++23 mode; the
project still uses `-std=c++23`. Meson checks support at configure time.

```sh
meson setup build
meson compile -C build
build/nxt-tests
```

New build directories default to `debugoptimized`: optimization and debug
symbols, with assertions enabled. Use `meson setup build --buildtype=debug`
for an unoptimized build, or `meson configure build -Dbuildtype=debugoptimized`
to update an existing debug build.

The default build produces `nxt-tests`, `nxtllm`, `nxtmt`, `wisp`, the shared
`libnxt-core`, the demo programs, and the `nxt-dev` developer command bundle.
Try the small TUI demo with:

```sh
build/demo/nxt-tui-demo
```

`meson install` gives a self-contained install: the libraries, tools, demos,
public headers (with the vendored libvterm/mdspan/hub headers under
`include/nxt-vendor`), and an `nxt.pc` for pkg-config. Consumers also need
Boost headers and `-std=c++23`.

`libcrypto` is required for certificate verification; AWS-LC is provided by
Nix, and OpenSSL is also supported for X.509 verification. The crypto
cross-check tests (RSA/ECDSA fixtures and ML-KEM-768) expect AWS-LC's headers;
with OpenSSL those optional cross-checks are skipped at configure time.

Coroutine frames use the ordinary C++ allocator (see
[RFC 0002](https://github.com/mbrock/nxtui/blob/main/rfc/cur/rfc-0002-firm-frame-arenas.md) for the removed firm arenas).

Meson keeps allocator poisoning enabled for everyday tests and the repeated
HTTP stress tier. To run the stress cases with poisoning explicitly:

```sh
MALLOC_PERTURB_=17 build/nxt-tests --only-slow
```

## Continuous integration

[GitHub Actions](https://github.com/mbrock/nxtui/actions/workflows/tests.yml)
runs on pushes and pull requests with the locked Nix development toolchain:

| Runner | Application Wand |
| --- | --- |
| Ubuntu 24.04 | io_uring |
| Ubuntu 24.04 | epoll |
| macOS 15 (Apple Silicon) | kqueue |

Each job runs `meson test`: the `nxt-tests` binary, its slow tier
(`nxt-slow-tests`), Wisp allocation-failure tests, and the Wisp host and HTTP
integration tests (also in the `slow` suite). Linux also includes the direct
epoll and io_uring suites; macOS includes the kqueue suite. Meson logs are
uploaded even on failure. Builds use `debugoptimized` (optimization plus
debug symbols, with assertions still enabled); the test harness's per-test
deadlines remain intact. Backends are not silently skipped or substituted.

To reproduce a matrix leg inside `nix develop`:

```sh
meson setup build/ci --buildtype=debugoptimized -Ddefault_wand=epoll -Ddemo=false -Ddev=false -Dllm_tool=false -Dcares=enabled
meson compile -C build/ci -j 2 nxt-tests wisp-alloc-tests wisp-root-link
meson test -C build/ci --print-errorlogs --timeout-multiplier 3
```

Use `uring` on Linux or `kqueue` on BSD/macOS instead of `epoll`. The default
`auto` retains io_uring on Linux and kqueue on BSD/macOS. The selected default
is propagated to library consumers through the Meson dependency and
pkg-config flags, since the application runtime contains a concrete Wand.

## Wisp coverage (GCC)

Use a separate instrumented build, inside `nix develop` or with a matching
GCC/Meson toolchain. This does not change the normal build's flags:

```sh
meson setup build/coverage -Db_coverage=true -Ddemo=false -Ddev=false -Dllm_tool=false
meson compile -C build/coverage nxt-tests wisp-alloc-tests wisp-root-link
find build/coverage -name '*.gcda' -delete  # reset counters before each comparison
build/coverage/test/nxt-tests 14 15 16 17 18 19 20 21
build/coverage/test/wisp-alloc-tests
gcov --json-format -b -c --stdout build/coverage/src/libnxt-core.so.p/wisp_*.gcno \
  build/coverage/test/wisp-alloc-tests.p/.._src_wisp_heap.cpp.gcno \
  > build/coverage/wisp-coverage.jsonl
```

The selectors above cover the Wisp suites in the nested test report. The
JSON-lines report includes line counts and branch outcomes for the core, not
the console host; add `meson test -C build/coverage wisp-host-tests wisp-http-tests`
to exercise host integration as well. Include both heap objects: allocation
fault injection runs in its own executable. Template instantiations and
separate objects can repeat source lines, so sum their execution counts by file
and line before computing line coverage. Uncovered exception paths and
compiler-generated branches are not necessarily missing language tests: use
the report to find contracts worth testing, not as a percentage target. Compare
the same workload with fresh counters, and assert results and snapshot
isolation, not just successful execution.

## With Nix

The flake is optional and wraps the same Meson build:

```sh
nix build            # ./result: libnxt-core, headers, nxt.pc, nxtllm, demos
nix flake check      # the package (with tests) plus a pkg-config consumer build
nix develop          # Clang, Meson, AWS-LC, clangd, docs tools, Racket + JDK
nix develop .#clang  # explicit Clang shell (also the default on Linux and macOS)
nix develop .#gcc    # GCC 16 and libstdc++ from the pinned Nixpkgs
nix develop .#filc   # experimental Fil-C compiler and libraries (Linux only)
```

Inside `nix develop`, the plain C++ build/test and docs commands above work
as-is. `.envrc` uses this flake through direnv and then loads `.env` if present.
Meson keeps the compiler chosen at setup time; use a new build directory when
switching toolchains. The Clang and GCC shells include the cached Racket and
Java environment for runtime model development.

To try a compiler without entering a shell manually:

```sh
make clang      # configure build/clang if needed, then build nxt-dev
make clang-test # build and run the tests, including slow suites
make gcc        # configure build/gcc if needed, then build nxt-dev
make gcc-test   # build and run the tests, including slow suites
make filc       # try building nxt-dev with Fil-C in build/filc (Linux)
make filc-test  # build and run the tests with Fil-C
```

The Clang commands use `nix develop .#clang` and a separate `build/clang`
directory; override it with `CLANG_BUILD_DIR=...`. Inside that shell,
`make build-clang` and `make test-clang` run the same workflow.

The GCC commands use `nix develop .#gcc` and a separate `build/gcc` directory.
Inside that shell, `make build-gcc` and `make test-gcc` run the same workflow.
Override the directory with `GCC_BUILD_DIR=...` if needed. The older
`build-gcc15` / `test-gcc15` targets are for separately installed compilers
named `gcc-15` / `g++-15`.

The experimental Fil-C commands use `nix develop .#filc` and `build/filc`;
override the directory with `FILC_BUILD_DIR=...`. Inside that shell, use
`make build-filc` and `make test-filc`. The pinned `github:mbrock/filnix`
input supplies both the compiler and its matching library package set, since
Fil-C libraries must use the Fil-C ABI. AWS-LC uses its portable C mode because
its assembly flags are not supported by Fil-C's assembler. The shell uses
Fil-C's linker and contains the C++ build tools; use the default shell for docs
and runtime model work. `nix build .#nxt-filc` attempts the installable package with tests.

At the pinned Filnix revision `0e32960446f8`, the x86_64 Linux attempt is
blocked before Meson setup: AWS-LC 1.69.0 compiles in portable mode, but its
`ASN1Test.ASN1Dup` test triggers `filc safety error: attempting to use
unrecognized madvise advice -1`. AWS-LC's fork-detection code deliberately
passes invalid advice to probe for an older QEMU bug; Fil-C traps on that
argument. Dependency checks remain enabled. Nxt compilation and runtime
compatibility have not yet been established with this toolchain.

For editors launched outside the development shell, use `scripts/clangd` as
the language-server executable. It enters the pinned Nix shell so clangd sees
the same compiler version, standard library, and dependency headers as the
build. `.clangd` already points to Meson's `build/compile_commands.json`;
the database alone does not include every header path supplied by Nix's
compiler wrappers.

In Zed, add this to the project's `.zed/settings.json`, replacing the path
with the absolute path to your checkout (keep this machine-specific setting
local):

```json
{
  "lsp": {
    "clangd": {
      "binary": {
        "path": "/absolute/path/to/nxtui/scripts/clangd",
        "arguments": []
      }
    }
  }
}
```

Then run `editor: restart language server` from Zed's command palette.
Keep a configured Meson `build` directory, as with the normal build workflow.

## In Amp orbs

`.agents/setup` installs Nix, realizes the development shell from `flake.lock`,
and configures Meson. The packaged Poxy generator, Racket, Java, and the
compiled model dependencies are included in the development shell. Setup does not compile
the editable model sources. Amp snapshots the base dependencies for
reuse by fresh orbs. Repeated setup runs reuse installed packages;
`.agents/resume` does not install anything.

Setup adds a repository-scoped login-shell hook so agents can run `make build`,
`make test`, and `make docs` directly from the repository root, without manually
entering `nix develop`. Run model checks with `make spec` in the same
environment. No API credentials are needed for these local
workflows.

The orb's host kernel also matters. Subprocess waits use `io_uring` to poll
pidfds, then reap ready children with ordinary `waitid(P_PIDFD)`. This works
on Linux 6.1 without `IORING_OP_WAITID` (which requires Linux 6.7); the pidfd
wait mechanism itself requires Linux 5.4 or later. The `io_uring` backend
still requires io_uring to be enabled and permitted by the host; there is no
automatic switch to epoll if ring creation is unavailable.

On macOS and the BSDs, the kqueue backend runs the same process wishes
without pidfds. A child's handle is its pid, kept reserved by reading exit
status with `waitid(WNOWAIT)` and reaping only when the handle is destroyed,
so a signal can never reach a reused pid. Waits register `EVFILT_PROC`
`NOTE_EXIT`; a process caught mid-exit refuses that registration (`ESRCH`),
and the wait retries on a short timer. One difference remains: waiting twice
reports the same status again, where Linux reports `ECHILD`. On macOS,
`posix_spawn` uses `POSIX_SPAWN_CLOEXEC_DEFAULT`, so children inherit only
their standard streams. All wands share the spawn code in
[`nxtrt/spawn.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/spawn.hpp).

## The runtime spec

`make spec` checks the Racket model (`nxtrt/runtime.rkt` and friends): its
example scenarios must be satisfiable and its lifecycle properties must hold
(for instance, that an exec only retires once its cancel CQE has drained, as
`is_retirable` requires); `make spec-witnesses` prints the example traces. It
needs Racket and a Java runtime (Forge's Pardinus solver runs on the JVM);
enter `nix develop` or run:

```sh
nix develop -c make spec
```

Racket, Java, and the compiled Racket libraries all come from Nix.
On Linux the spec environment uses `racket-minimal`, avoiding the full Racket
distribution's GTK and desktop wrappers. Darwin retains full Racket because
the pinned Nixpkgs marks its minimal package broken there.
`nix/racket-sources.json` pins the external package closure to source revisions
and Nix content hashes; `nix/spec-racket.nix` installs and compiles those inputs
offline. `nix/spec-sources.nix` fetches Forge v5.2 and Something at pinned
upstream commits and applies the small patches in `nix/patches/`; their full
source trees are not checked into this repo.
The resulting `spec-racket` package is an ordinary cacheable Nix derivation:

```sh
nix build .#spec-racket
```

This is a spec backend environment: it compiles Forge's functional API,
temporal checks, XML serializer, and Something's reader, with their imports.
The patches preserve XML export and reader tokenization, make Git metadata
optional, disable online Forge version checks, omit two embedded Typed RackUnit
test submodules that pull in GUI dependencies, and narrow package dependencies
to the supported backend modules. GUI/editor tooling, documentation packages,
Forge's domain examples, and Something's experimental shells are outside this
environment. The dependency lock contains no GUI or drawing packages. Linux
uses minimal Racket; Darwin still uses full Racket because Nixpkgs marks its
minimal package broken there.

The dependency inputs are reproducibly locked and the output can be shared
through a normal Nix binary cache. Byte-for-byte rebuild reproducibility is
not guaranteed: `nix build .#spec-racket --rebuild` found differences in
Racket-generated `.zo` files and their dependency hashes, even with serial
compilation. This does not prevent substitution of a cached build.

No catalog resolution or package installation happens when entering the shell
or running `make spec`. Only bytecode for editable model/DSL sources goes into
the version-keyed `.racket/` cache; old local package installs there are ignored.
Editing a model does not rebuild the dependency package. `nix flake check`
also runs the specs in a clean Nix build sandbox.

To deliberately refresh the dependency lock from the Racket catalog (this is
the networked update step, not part of normal builds), the updater prefers the
catalog for the installed Racket version, then the community catalog. It locks
every runtime dependency not supplied by minimal Racket, including libraries that
would otherwise be bundled with the full distribution:

```sh
spec_sources=$(nix build .#spec-sources --no-link --print-out-paths)
nix develop -c racket nix/update-racket-sources.rkt "$spec_sources" > nix/racket-sources.json.new &&
  mv nix/racket-sources.json.new nix/racket-sources.json
nix build .#checks.x86_64-linux.spec # use your system's check attribute
```

Review and commit the lock changes. The updater derives the closure from the
patched packages' dependency declarations and the runtime declarations in
each fetched dependency. Catalog build/documentation dependencies are excluded.
Only `base` and `racket-lib`, supplied by minimal Racket, are omitted from the
lock; this keeps it complete even when updated using Darwin's full distribution.

To change an upstream revision or a backend patch, edit `nix/spec-sources.nix`
or `nix/patches/`, regenerate the lock with the command above if dependencies
changed, and run `nix develop -c make spec` plus the sandboxed spec check.

## API docs

API docs are published automatically to [mbrock.github.io/nxtui](https://mbrock.github.io/nxtui/)
on every push to `main` by `.github/workflows/docs.yml`. The workflow builds
with the docs-only Nix shell and Poxy pinned in `nix/poxy.nix`, including
Doxygen from `flake.lock`, then deploys the generated HTML through GitHub Pages.
Poxy and its Python dependencies are built by Nix; generating docs does not
install packages from PyPI. Hestia caches the docs toolchain between Actions
runs, with daily cleanup in `.github/workflows/cache-gc.yml`.

Regenerate local API docs with:

```sh
nix develop .#docs -c make docs
```

To request another publication of the committed `main` branch, run
`make docs-publish` with an authenticated GitHub CLI, or use the workflow's
**Run workflow** button on GitHub.

## FreeBSD VM

Run the portable test subset on a FreeBSD VM with:

```sh
scripts/freebsd-vm init
scripts/freebsd-vm test
```

The helper uses libvirt, cloud-init, ssh, and rsync. It defaults to the official
FreeBSD 15.0 amd64 `BASIC-CLOUDINIT-ufs.qcow2.xz` image, stores local state
under `.cache/freebsd-vm`, and leaves the VM persistent for fast repeat runs.
The guest installs GCC 15 and uses `gcc15`/`g++15` for the test build. The host
needs `libvirt-daemon-system`, `virtinst`, and `cloud-image-utils`.
