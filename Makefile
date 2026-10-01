.PHONY: all setup setup-unity build dev full test setup-gcc13 build-gcc13 test-gcc13 gcc13 freebsd-test deps deps-dot bench-build bench bench-plain bench-residency bench-perf bench-perf-report bench-perf-hot bench-perf-duck bench-uring-stat bench-uring-record bench-uring-duck bench-uring-trace setup-racket spec spec-witnesses docs docs-publish clean traces

BENCH_BUILD_DIR ?= build-bench-release
BENCH_BIN ?= $(BENCH_BUILD_DIR)/bench/nxt-echo-bench
BENCH_CPP_ARGS ?=
GCC13_BUILD_DIR ?= build-gcc13
DEPS_FILE ?=
DEPS_DEPTH ?= 4
DEPS_FLAGS ?=
NXT_MESON_LINK_ARGS ?= $(shell command -v mold >/dev/null 2>&1 && printf '%s' '-Dc_link_args=-fuse-ld=mold -Dcpp_link_args=-fuse-ld=mold')
NXT_RACKET_COLLECTS := $(CURDIR):$(CURDIR)/vendor/racket:$(CURDIR)/vendor/racket/something-src
# Racket packages for the spec live in the project, not in the user's global
# Racket dir. Racket keys this by version, so upgrading Racket just means
# re-running setup-racket (which `make spec` does on its own).
# Compiled .zo files for repo sources go under the same version-keyed dir
# instead of in-tree compiled/ dirs, so two Racket versions never collide.
NXT_RACKET_ADDON_DIR ?= $(CURDIR)/.racket
NXT_RACKET_VERSION_DIR = $(NXT_RACKET_ADDON_DIR)/$(shell racket -e '(display (version))')
NXT_RACKET_STAMP = $(NXT_RACKET_VERSION_DIR)/nxt-setup.stamp
NXT_RACKET_ENV = PLTADDONDIR="$(NXT_RACKET_ADDON_DIR)" PLTCOMPILEDROOTS="$(NXT_RACKET_VERSION_DIR)/compiled:same" PLTCOLLECTS="$(NXT_RACKET_COLLECTS):$${PLTCOLLECTS}"
NXT_RACKET = $(NXT_RACKET_ENV) racket
NXT_RACO := PLTADDONDIR="$(NXT_RACKET_ADDON_DIR)" raco
NXT_SPEC_SOURCES := rdf-forge/tests/bfo-sketch-test.rkt nxtrt/model.rkt nxtrt/model-next.rkt
NXT_RACKET_PKGS := syntax-classes br-parser-tools-lib brag-lib beautiful-racket crypto-lib mischief pretty-format predicates basedir request sha http-easy tabular

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

setup-gcc13:
	CC=gcc-13 CXX=g++-13 meson setup "$(GCC13_BUILD_DIR)" $(NXT_MESON_LINK_ARGS)

build-gcc13:
	@if [ ! -d "$(GCC13_BUILD_DIR)" ]; then \
		CC=gcc-13 CXX=g++-13 meson setup "$(GCC13_BUILD_DIR)" $(NXT_MESON_LINK_ARGS); \
	fi
	meson compile -C "$(GCC13_BUILD_DIR)" nxt-dev

test-gcc13: build-gcc13
	meson compile -C "$(GCC13_BUILD_DIR)" nxt-tests
	meson test -C "$(GCC13_BUILD_DIR)"

gcc13: test-gcc13

deps:
	@scripts/include-graph --summary --depth "$(DEPS_DEPTH)" $(DEPS_FLAGS) $(DEPS_FILE)

deps-dot:
	@mkdir -p build
	@scripts/include-graph --dot > build/include-graph.dot
	@printf "wrote build/include-graph.dot\n"

freebsd-test:
	scripts/freebsd-vm test

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

setup-racket:
	$(NXT_RACO) pkg install --auto --batch --skip-installed -D $(NXT_RACKET_PKGS)
	$(NXT_RACO) pkg install --auto --batch --skip-installed --no-setup -D --link -n something vendor/racket/something-src
	$(NXT_RACO) pkg install --auto --batch --skip-installed --no-setup -D --link -n forge vendor/racket/forge
	$(NXT_RACO) pkg install --auto --batch --skip-installed --no-setup -D --link -n rdf-forge rdf-forge
	@touch "$(NXT_RACKET_STAMP)"

spec:
	@test -f "$(NXT_RACKET_STAMP)" || $(MAKE) setup-racket
	$(NXT_RACKET_ENV) raco make -j 4 $(NXT_SPEC_SOURCES)
	@echo
	@echo "== rdf-forge ontology syntax =="
	$(NXT_RACKET) rdf-forge/tests/bfo-sketch-test.rkt
	@echo
	@echo "== baseline runtime spec =="
	$(NXT_RACKET) nxtrt/model.rkt --check
	@echo
	@echo "== next runtime spec =="
	$(NXT_RACKET) nxtrt/model-next.rkt --check

# Print the example traces from every run block instead of checking.
spec-witnesses:
	@test -f "$(NXT_RACKET_STAMP)" || $(MAKE) setup-racket
	$(NXT_RACKET_ENV) raco make -j 4 $(NXT_SPEC_SOURCES)
	$(NXT_RACKET) nxtrt/model.rkt --run-all
	$(NXT_RACKET) nxtrt/model-next.rkt --run-all

docs:
	rm -rf docs/html
	mkdir -p docs/html
	uvx poxy --output-dir docs docs/poxy.toml
	chmod -R a+rX docs/html

docs-publish: docs
	@if [ "$$(readlink /var/www/nxt 2>/dev/null)" != "$(CURDIR)/docs/html" ]; then \
		sudo ln -sfnT $(CURDIR)/docs/html /var/www/nxt; \
	fi

clean:
	rm -rf build "$(BENCH_BUILD_DIR)"
