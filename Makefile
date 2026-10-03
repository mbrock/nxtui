.PHONY: all setup setup-unity build dev full test setup-gcc15 build-gcc15 test-gcc15 gcc15 freebsd-test deps deps-dot bench-build bench bench-plain bench-residency bench-perf bench-perf-report bench-perf-hot bench-perf-duck bench-uring-stat bench-uring-record bench-uring-duck bench-uring-trace spec spec-witnesses docs docs-publish clean

POXY_VERSION ?= 0.27.1

BENCH_BUILD_DIR ?= build-bench-release
BENCH_BIN ?= $(BENCH_BUILD_DIR)/bench/nxt-echo-bench
BENCH_CPP_ARGS ?=
WISP_BENCH_BUILD_DIR ?= build/wisp-release
WISP_PROFILE ?= false
WISP_BENCH_ARGS ?=
GCC15_BUILD_DIR ?= build-gcc15
GCC_BUILD_DIR ?= build/gcc
CLANG_BUILD_DIR ?= build/clang
DEPS_FILE ?=
DEPS_DEPTH ?= 4
DEPS_FLAGS ?=
NXT_MESON_LINK_ARGS ?= $(shell command -v mold >/dev/null 2>&1 && printf '%s' '-Dc_link_args=-fuse-ld=mold -Dcpp_link_args=-fuse-ld=mold')
# Run in `nix develop`: packages and their bytecode live in the Nix
# store. Only editable repo sources use a local, version-keyed bytecode cache.
NXT_RACKET_CACHE_DIR ?= $(CURDIR)/.racket
NXT_RACKET_VERSION_DIR = $(NXT_RACKET_CACHE_DIR)/$(shell racket -e '(display (version))')
# Empty suffixes retain the installed collection and bytecode search paths.
NXT_RACKET_ENV = PLTCOMPILEDROOTS="$(NXT_RACKET_VERSION_DIR)/compiled:" PLTCOLLECTS="$(CURDIR):"
NXT_RACKET = $(NXT_RACKET_ENV) racket
NXT_SPEC_SOURCES := rdf-forge/tests/bfo-sketch-test.rkt nxtrt/model.rkt

all: build

setup:
	meson setup build $(NXT_MESON_LINK_ARGS)

setup-unity:
	meson setup build -Dunity=on $(NXT_MESON_LINK_ARGS)

build:
	meson compile -C build nxt-dev

dev: build

full:
	meson compile -C build

test:
	meson compile -C build nxt-tests
	meson test -C build

setup-gcc15:
	CC=gcc-15 CXX=g++-15 meson setup "$(GCC15_BUILD_DIR)" $(NXT_MESON_LINK_ARGS)

build-gcc15:
	@if [ ! -d "$(GCC15_BUILD_DIR)" ]; then \
		CC=gcc-15 CXX=g++-15 meson setup "$(GCC15_BUILD_DIR)" $(NXT_MESON_LINK_ARGS); \
	fi
	meson compile -C "$(GCC15_BUILD_DIR)" nxt-dev

test-gcc15: build-gcc15
	meson compile -C "$(GCC15_BUILD_DIR)" nxt-tests
	meson test -C "$(GCC15_BUILD_DIR)"

gcc15: test-gcc15

# One-command entry points also work outside a Nix development shell.
.PHONY: gcc gcc-test setup-gcc build-gcc test-gcc
gcc:
	nix develop .#gcc -c make build-gcc GCC_BUILD_DIR="$(GCC_BUILD_DIR)"

gcc-test:
	nix develop .#gcc -c make test-gcc GCC_BUILD_DIR="$(GCC_BUILD_DIR)"

setup-gcc:
	meson setup "$(GCC_BUILD_DIR)" $(NXT_MESON_LINK_ARGS)

build-gcc:
	@if [ ! -f "$(GCC_BUILD_DIR)/build.ninja" ]; then \
		meson setup "$(GCC_BUILD_DIR)" $(NXT_MESON_LINK_ARGS); \
	fi
	meson compile -C "$(GCC_BUILD_DIR)" nxt-dev

test-gcc: build-gcc
	meson compile -C "$(GCC_BUILD_DIR)" nxt-tests
	meson test -C "$(GCC_BUILD_DIR)"

.PHONY: clang clang-test setup-clang build-clang test-clang
clang:
	nix develop .#clang -c make build-clang CLANG_BUILD_DIR="$(CLANG_BUILD_DIR)"

clang-test:
	nix develop .#clang -c make test-clang CLANG_BUILD_DIR="$(CLANG_BUILD_DIR)"

setup-clang:
	meson setup "$(CLANG_BUILD_DIR)" $(NXT_MESON_LINK_ARGS)

build-clang:
	@if [ ! -f "$(CLANG_BUILD_DIR)/build.ninja" ]; then \
		meson setup "$(CLANG_BUILD_DIR)" $(NXT_MESON_LINK_ARGS); \
	fi
	meson compile -C "$(CLANG_BUILD_DIR)" nxt-dev

test-clang: build-clang
	meson compile -C "$(CLANG_BUILD_DIR)" nxt-tests
	meson test -C "$(CLANG_BUILD_DIR)"

deps:
	@scripts/include-graph --summary --depth "$(DEPS_DEPTH)" $(DEPS_FLAGS) $(DEPS_FILE)

deps-dot:
	@mkdir -p build
	@scripts/include-graph --dot > build/include-graph.dot
	@printf "wrote build/include-graph.dot\n"

freebsd-test:
	scripts/freebsd-vm test

.PHONY: wisp-bench-build wisp-bench wisp-bench-sweep
wisp-bench-build:
	meson setup --reconfigure "$(WISP_BENCH_BUILD_DIR)" \
		--buildtype=release -Db_ndebug=true -Dbenchmarks=true \
		-Dwisp_profile=$(WISP_PROFILE) -Dtests=false -Ddemo=false \
		-Ddev=false -Dllm_tool=false -Dwisp_tool=false $(NXT_MESON_LINK_ARGS)
	meson compile -C "$(WISP_BENCH_BUILD_DIR)" wisp-bench wisp-native-bench wisp-profile-test
	meson test -C "$(WISP_BENCH_BUILD_DIR)" --suite benchmarks --print-errorlogs

wisp-bench: wisp-bench-build
	"$(WISP_BENCH_BUILD_DIR)/bench/wisp-bench" $(WISP_BENCH_ARGS)

wisp-bench-sweep: wisp-bench-build
	python3 scripts/wisp-bench --build-dir "$(WISP_BENCH_BUILD_DIR)" $(WISP_BENCH_ARGS)

bench-build:
	@if [ ! -d "$(BENCH_BUILD_DIR)" ]; then \
		meson setup "$(BENCH_BUILD_DIR)" \
			--buildtype=release \
			-Dbenchmarks=true \
			-Dtests=false \
			-Ddemo=false \
			-Dllm_tool=false \
			-Dcpp_args='$(BENCH_CPP_ARGS)' \
			$(NXT_MESON_LINK_ARGS); \
	else \
		meson configure "$(BENCH_BUILD_DIR)" \
			-Dbenchmarks=true \
			-Dtests=false \
			-Ddemo=false \
			-Dllm_tool=false \
			-Dcpp_args='$(BENCH_CPP_ARGS)' \
			$(NXT_MESON_LINK_ARGS); \
	fi
	meson compile -C "$(BENCH_BUILD_DIR)" "$(notdir $(BENCH_BIN))"

bench: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench all

bench-plain: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench plain

bench-residency: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench residency

bench-perf: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench perf-record

bench-perf-report: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench perf-report

bench-perf-hot: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench perf-hot

bench-perf-duck: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench perf-duck

bench-uring-stat: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench uring-stat

bench-uring-record: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench uring-record

bench-uring-duck: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench uring-duck

bench-uring-trace: bench-build
	@BENCH_BIN="$(BENCH_BIN)" scripts/bench uring-trace

spec:
	$(NXT_RACKET_ENV) raco make -j 4 $(NXT_SPEC_SOURCES)
	@echo
	@echo "== rdf-forge ontology syntax =="
	$(NXT_RACKET) rdf-forge/tests/bfo-sketch-test.rkt
	@echo
	@echo "== baseline runtime spec =="
	$(NXT_RACKET) nxtrt/model.rkt --check

# Print the example traces from every run block instead of checking.
spec-witnesses:
	$(NXT_RACKET_ENV) raco make -j 4 $(NXT_SPEC_SOURCES)
	$(NXT_RACKET) nxtrt/model.rkt --run-all

docs:
	rm -rf docs/html
	mkdir -p docs/html
	uvx --from 'poxy==$(POXY_VERSION)' poxy --output-dir docs docs/poxy.toml
	chmod -R a+rX docs/html

docs-publish:
	gh workflow run docs.yml --ref main

clean:
	rm -rf build "$(BENCH_BUILD_DIR)"
