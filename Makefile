.PHONY: all setup build full test freebsd-test bench-build bench bench-perf bench-perf-report bench-perf-hot bench-perf-duck spec docs docs-publish clean traces

BENCH_CLIENTS ?= 16
BENCH_MESSAGES ?= 512
BENCH_PAYLOAD ?= 64
BENCH_CPP_ARGS ?= -DNXT_RT_ENABLE_TRACE=0
BENCH_PERF_DATA ?= build-bench-release/nxt-echo.perf.data
BENCH_PERF_FREQ ?= 999
BENCH_PERF_CALLGRAPH ?= fp
BENCH_PERF_CLIENTS ?= 64
BENCH_PERF_MESSAGES ?= 2048
BENCH_PERF_PAYLOAD ?= 64
BENCH_PERF_HOT_LIMIT ?= 0.5
BENCH_PERF_HOT_LINES ?= 40
BENCH_PERF_SYMBOL_WIDTH ?= 96
BENCH_PERF_DUCK_JSON ?= build-bench-release/nxt-echo.perf.json
BENCH_PERF_DUCK_LINES ?= 20
BENCH_PERF_TREE_DEPTH ?= 9
BENCH_PERF_TREE_CHILDREN ?= 6
BENCH_PERF_TREE_MIN_PCT ?= 1.0

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
	@if [ ! -d build-bench-release ]; then \
		meson setup build-bench-release --buildtype=release -Dbenchmarks=true -Dtests=false -Ddemo=false -Dllm_tool=false -Dcpp_args=$(BENCH_CPP_ARGS); \
	fi
	meson configure build-bench-release -Dcpp_args=$(BENCH_CPP_ARGS)
	meson compile -C build-bench-release nxt-echo-bench

bench: bench-build
	build-bench-release/nxt-echo-bench --clients $(BENCH_CLIENTS) --messages $(BENCH_MESSAGES) --payload $(BENCH_PAYLOAD)

bench-perf: bench-build
	sudo perf record -F $(BENCH_PERF_FREQ) -g --call-graph $(BENCH_PERF_CALLGRAPH) -o $(BENCH_PERF_DATA) -- build-bench-release/nxt-echo-bench --clients $(BENCH_PERF_CLIENTS) --messages $(BENCH_PERF_MESSAGES) --payload $(BENCH_PERF_PAYLOAD)

bench-perf-report:
	sudo perf report -i $(BENCH_PERF_DATA)

bench-perf-hot:
	@sudo perf report -i $(BENCH_PERF_DATA) --stdio --stdio-color never \
		--no-children --no-call-graph --fields overhead,dso,symbol \
		--percent-limit $(BENCH_PERF_HOT_LIMIT) -t '|' 2>/dev/null \
		| awk -F'|' 'BEGIN { \
			printf "%-8s %-18s %s\n", "Overhead", "DSO", "Symbol"; \
			printf "%-8s %-18s %s\n", "--------", "---", "------" \
		} /^[[:space:]]*[0-9]/ { \
			gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$1); \
			gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$2); \
			gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$3); \
			if (length($$3) > $(BENCH_PERF_SYMBOL_WIDTH)) \
				$$3 = substr($$3, 1, $(BENCH_PERF_SYMBOL_WIDTH) - 3) "..."; \
			printf "%-8s %-18s %s\n", $$1, $$2, $$3 \
		}' \
		| head -$(BENCH_PERF_HOT_LINES)

bench-perf-duck:
	@BENCH_PERF_DUCK_LINES=$(BENCH_PERF_DUCK_LINES) BENCH_PERF_SYMBOL_WIDTH=$(BENCH_PERF_SYMBOL_WIDTH) BENCH_PERF_TREE_DEPTH=$(BENCH_PERF_TREE_DEPTH) BENCH_PERF_TREE_CHILDREN=$(BENCH_PERF_TREE_CHILDREN) BENCH_PERF_TREE_MIN_PCT=$(BENCH_PERF_TREE_MIN_PCT) scripts/perf-duckdb $(BENCH_PERF_DATA) $(BENCH_PERF_DUCK_JSON)

spec:
	racket nxtrt/model.rkt --run-all

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
	rm -rf build build-bench-release
