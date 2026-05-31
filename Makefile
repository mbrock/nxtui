.PHONY: all setup build full test freebsd-test bench-build bench bench-plain bench-residency bench-perf bench-perf-report bench-perf-hot bench-perf-duck bench-uring-stat bench-uring-record bench-uring-duck bench-uring-trace spec docs docs-publish clean traces

BENCH_BUILD_DIR ?= build-bench-release
BENCH_BIN ?= $(BENCH_BUILD_DIR)/nxt-echo-bench
BENCH_CPP_ARGS ?=

all: build

setup:
	meson setup build

build:
	meson compile -C build nxtllm

full:
	meson compile -C build

test:
	meson compile -C build nxt-tests
	meson test -C build

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
			-Dcpp_args='$(BENCH_CPP_ARGS)'; \
	else \
		meson configure "$(BENCH_BUILD_DIR)" \
			-Dbenchmarks=true \
			-Dtests=false \
			-Ddemo=false \
			-Dllm_tool=false \
			-Dcpp_args='$(BENCH_CPP_ARGS)'; \
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
	@echo "== rdf-forge ontology syntax =="
	racket rdf-forge/tests/bfo-sketch-test.rkt
	@echo
	@echo "== baseline runtime spec =="
	racket nxtrt/model.rkt --run-all
	@echo
	@echo "== next runtime spec =="
	racket nxtrt/model-next.rkt --run-all

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
