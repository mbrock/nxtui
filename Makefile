.PHONY: all setup build full test freebsd-test bench spec docs docs-publish clean traces

BENCH_CLIENTS ?= 16
BENCH_MESSAGES ?= 512
BENCH_PAYLOAD ?= 64

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

bench:
	@if [ ! -d build-bench-release ]; then \
		meson setup build-bench-release --buildtype=release -Dbenchmarks=true -Dtests=false -Ddemo=false -Dllm_tool=false; \
	else \
		meson setup build-bench-release --reconfigure -Dbenchmarks=true -Dtests=false -Ddemo=false -Dllm_tool=false; \
	fi
	meson compile -C build-bench-release nxt-echo-bench
	build-bench-release/nxt-echo-bench --clients $(BENCH_CLIENTS) --messages $(BENCH_MESSAGES) --payload $(BENCH_PAYLOAD)

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
